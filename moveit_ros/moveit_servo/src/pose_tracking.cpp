/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2020, PickNik LLC
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of PickNik LLC nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

#include "moveit_servo/pose_tracking.h"

namespace
{
constexpr char LOGNAME[] = "pose_tracking";
constexpr double DEFAULT_LOOP_RATE = 100;     // Hz
constexpr double ROS_STARTUP_WAIT = 10;       // sec
constexpr double DEFAULT_POSE_TIMEOUT = 0.1;  // sec
}

namespace moveit_servo
{
PoseTracking::PoseTracking(const planning_scene_monitor::PlanningSceneMonitorPtr& planning_scene_monitor,
                           const std::string& parameter_ns)
  : planning_scene_monitor_(planning_scene_monitor)
  , loop_rate_(DEFAULT_LOOP_RATE)
  , transform_listener_(transform_buffer_)
  , stop_requested_(false)
  , parameter_ns_(parameter_ns)
  , angular_error_(0)
{
  readROSParams();

  robot_model_ = planning_scene_monitor_->getRobotModel();
  joint_model_group_ = robot_model_->getJointModelGroup(move_group_name_);

  // Initialize PID controllers
  initializePID(x_pid_config_, cartesian_position_pids_);
  initializePID(y_pid_config_, cartesian_position_pids_);
  initializePID(z_pid_config_, cartesian_position_pids_);
  initializePID(angular_pid_config_, cartesian_orientation_pids_);

  // Use the C++ interface that Servo provides
  servo_ = std::make_unique<moveit_servo::Servo>(nh_, planning_scene_monitor_, parameter_ns_);
  servo_->start();

  // Connect to Servo ROS interfaces
  std::string target_pose_topic = "/" + parameter_ns_ + "/target_pose";
  target_pose_sub_ =
      nh_.subscribe<geometry_msgs::PoseStamped>(target_pose_topic, 1, &PoseTracking::targetPoseCallback, this);

  // Publish outgoing twist commands to the Servo object
  twist_stamped_pub_ =
      nh_.advertise<geometry_msgs::TwistStamped>(servo_->getParameters().cartesian_command_in_topic, 1);
}

int8_t PoseTracking::moveToPose(const Eigen::Vector3d& positional_tolerance, const double angular_tolerance)
{
  // Roll back the target pose timestamp to ensure we wait for a new target pose message
  target_pose_.header.stamp = ros::Time::now() - ros::Duration(2 * DEFAULT_POSE_TIMEOUT);

  // Wait a bit for a target pose message to arrive.
  // The target pose may get updated by new messages as the robot moves (in a callback function).
  ros::Time start_time = ros::Time::now();
  while ((!haveRecentTargetPose(DEFAULT_POSE_TIMEOUT) || !haveRecentEndEffectorPose(DEFAULT_POSE_TIMEOUT)) &&
         ((ros::Time::now() - start_time).toSec() < DEFAULT_POSE_TIMEOUT))
  {
    if (servo_->getCommandFrameTransform(end_effector_transform_))
    {
      end_effector_transform_stamp_ = ros::Time::now();
    }
    ros::Duration(0.001).sleep();
  }
  if (!haveRecentTargetPose(DEFAULT_POSE_TIMEOUT))
  {
    ROS_ERROR_STREAM_NAMED(LOGNAME, "The target pose was not updated recently. Aborting.");
    return PoseTrackingStatusCode::NO_RECENT_TARGET_POSE;
  }

  while (ros::ok())
  {
    // Check for reasons to stop:
    // - Goal tolerance is satisfied
    // - Timeout
    // - Another thread requested a stop
    // - PID controllers aren't initialized
    if (satisfiesPoseTolerance(positional_tolerance, angular_tolerance))
    {
      break;
    }

    // Attempt to update robot pose
    if (servo_->getCommandFrameTransform(end_effector_transform_))
    {
      end_effector_transform_stamp_ = ros::Time::now();
    }

    if (!haveRecentEndEffectorPose(DEFAULT_POSE_TIMEOUT))
    {
      ROS_ERROR_STREAM_NAMED(LOGNAME, "The end effector pose was not updated in time. Aborting.");
      doPostMotionReset();
      return PoseTrackingStatusCode::NO_RECENT_END_EFFECTOR_POSE;
    }
    if (stop_requested_)
    {
      ROS_INFO_STREAM_NAMED(LOGNAME, "Halting servo motion, a stop was requested.");
      doPostMotionReset();
      return PoseTrackingStatusCode::STOP_REQUESTED;
    }

    // Compute servo command from PID controller output
    auto msg = calculateTwistCommand();

    // Send command to the Servo object, for execution
    twist_stamped_pub_.publish(*msg);
  }

  doPostMotionReset();
  return PoseTrackingStatusCode::SUCCESS;
}

void PoseTracking::readROSParams()
{
  std::size_t error = 0;

  // Check for parameter namespace from launch file. All other parameters will be read from this namespace.
  std::string yaml_namespace;
  if (ros::param::get("~parameter_ns", yaml_namespace))
  {
    if (!parameter_ns_.empty())
      ROS_WARN_STREAM_NAMED(LOGNAME,
                            "A parameter namespace was specified in the launch file AND in the constructor argument.");

    parameter_ns_ = yaml_namespace;
  }

  // Wait for ROS parameters to load
  ros::Time begin = ros::Time::now();
  while (!ros::param::has(parameter_ns_ + "/planning_frame") && ((ros::Time::now() - begin).toSec() < ROS_STARTUP_WAIT))
  {
    ROS_WARN_STREAM_NAMED(LOGNAME, "Waiting for parameter: " << parameter_ns_ + "/planning_frame");
    ros::Duration(0.1).sleep();
  }

  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/planning_frame", planning_frame_);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/move_group_name", move_group_name_);
  if (!planning_scene_monitor_->getRobotModel()->hasJointModelGroup(move_group_name_))
  {
    ++error;
    ROS_ERROR_STREAM_NAMED(LOGNAME, "Unable to find the specified joint model group: " << move_group_name_);
  }

  double publish_period;
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/publish_period", publish_period);
  loop_rate_ = ros::Rate(1 / publish_period);

  x_pid_config_.dt = publish_period;
  y_pid_config_.dt = publish_period;
  z_pid_config_.dt = publish_period;
  angular_pid_config_.dt = publish_period;

  double windup_limit;
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/windup_limit", windup_limit);
  x_pid_config_.windup_limit = windup_limit;
  y_pid_config_.windup_limit = windup_limit;
  z_pid_config_.windup_limit = windup_limit;
  angular_pid_config_.windup_limit = windup_limit;

  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/x_proportional_gain", x_pid_config_.k_p);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/y_proportional_gain", y_pid_config_.k_p);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/z_proportional_gain", z_pid_config_.k_p);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/x_integral_gain", x_pid_config_.k_i);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/y_integral_gain", y_pid_config_.k_i);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/z_integral_gain", z_pid_config_.k_i);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/x_derivative_gain", x_pid_config_.k_d);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/y_derivative_gain", y_pid_config_.k_d);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/z_derivative_gain", z_pid_config_.k_d);

  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/angular_proportional_gain", angular_pid_config_.k_p);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/angular_integral_gain", angular_pid_config_.k_i);
  error += !rosparam_shortcuts::get("", nh_, parameter_ns_ + "/angular_derivative_gain", angular_pid_config_.k_d);

  rosparam_shortcuts::shutdownIfError(ros::this_node::getName(), error);
}

void PoseTracking::initializePID(const PIDConfig& pid_config, std::vector<control_toolbox::Pid>& pid_vector)
{
  bool use_anti_windup = true;
  pid_vector.push_back(control_toolbox::Pid(pid_config.k_p, pid_config.k_i, pid_config.k_d, -pid_config.windup_limit,
                                            pid_config.windup_limit, use_anti_windup));
}

bool PoseTracking::haveRecentTargetPose(const double timespan)
{
  return ((ros::Time::now() - target_pose_.header.stamp).toSec() < timespan);
}

bool PoseTracking::haveRecentEndEffectorPose(const double timespan)
{
  return ((ros::Time::now() - end_effector_transform_stamp_).toSec() < timespan);
}

bool PoseTracking::satisfiesPoseTolerance(const Eigen::Vector3d& positional_tolerance, const double angular_tolerance)
{
  // TODO(andyz): check orientation too

  double x_error = target_pose_.pose.position.x - end_effector_transform_.translation()(0);
  double y_error = target_pose_.pose.position.y - end_effector_transform_.translation()(1);
  double z_error = target_pose_.pose.position.z - end_effector_transform_.translation()(2);

  return (fabs(x_error) < positional_tolerance(0)) && (fabs(y_error) < positional_tolerance(1)) &&
         (fabs(z_error) < positional_tolerance(2) && fabs(angular_error_) < angular_tolerance);
}

void PoseTracking::targetPoseCallback(geometry_msgs::PoseStamped msg)
{
  // Transform to MoveIt planning frame
  if (msg.header.frame_id != planning_frame_)
  {
    auto target_to_planning_frame =
        transform_buffer_.lookupTransform(planning_frame_, msg.header.frame_id, ros::Time(0), ros::Duration(0.1));
    tf2::doTransform(msg, msg, target_to_planning_frame);
  }
  target_pose_ = msg;
  target_pose_.header.stamp = ros::Time::now();
}

geometry_msgs::TwistStampedConstPtr PoseTracking::calculateTwistCommand()
{
  // use the shared pool to create a message more efficiently
  auto msg = moveit::util::make_shared_from_pool<geometry_msgs::TwistStamped>();
  msg->header.frame_id = target_pose_.header.frame_id;

  // Get twist components from PID controllers
  geometry_msgs::Twist& twist = msg->twist;

  // Position
  twist.linear.x = cartesian_position_pids_[0].computeCommand(
      target_pose_.pose.position.x - end_effector_transform_.translation()(0), loop_rate_.expectedCycleTime());
  twist.linear.y = cartesian_position_pids_[1].computeCommand(
      target_pose_.pose.position.y - end_effector_transform_.translation()(1), loop_rate_.expectedCycleTime());
  twist.linear.z = cartesian_position_pids_[2].computeCommand(
      target_pose_.pose.position.z - end_effector_transform_.translation()(2), loop_rate_.expectedCycleTime());

  // Orientation algorithm:
  // - Find the orientation error as a quaternion: q_error = q_desired * q_current ^ -1
  // - Use the quaternion PID controllers to calculate a quaternion rate, q_error_dot
  // - Convert to angular velocity for the TwistStamped message
  Eigen::Quaterniond q_desired(target_pose_.pose.orientation.w, target_pose_.pose.orientation.x,
                               target_pose_.pose.orientation.y, target_pose_.pose.orientation.z);
  Eigen::Quaterniond q_current(end_effector_transform_.rotation());
  Eigen::Quaterniond q_error = q_desired * q_current.inverse();

  // Convert axis-angle to angular velocity
  Eigen::AngleAxisd axis_angle(q_error);
  // Cache the angular error, for rotation tolerance checking
  angular_error_ = axis_angle.angle();
  double ang_vel_magnitude =
      cartesian_orientation_pids_[0].computeCommand(angular_error_, loop_rate_.expectedCycleTime());
  twist.angular.x = ang_vel_magnitude * axis_angle.axis()[0];
  twist.angular.y = ang_vel_magnitude * axis_angle.axis()[1];
  twist.angular.z = ang_vel_magnitude * axis_angle.axis()[2];

  msg->header.stamp = ros::Time::now();

  return msg;
}

void PoseTracking::doPostMotionReset()
{
  stop_requested_ = false;
  angular_error_ = 0;

  // Reset error integrals and previous errors of PID controllers
  cartesian_position_pids_[0].reset();
  cartesian_position_pids_[1].reset();
  cartesian_position_pids_[2].reset();
  cartesian_orientation_pids_[0].reset();
}
}  // namespace moveit_servo