#include <franka_ros_controllers/effort_joint_impedance_controller.h>

#include <cmath>
#include <memory>

#include <controller_interface/controller_base.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>

#include <franka/robot_state.h>

namespace franka_ros_controllers {

bool EffortJointImpedanceController::init(hardware_interface::RobotHW* robot_hw,
                                           ros::NodeHandle& node_handle) {
  std::string arm_id;
  if (!node_handle.getParam("arm_id", arm_id)) {
    ROS_ERROR("EffortJointImpedanceController: Could not read parameter arm_id");
    return false;
  }

  franka_state_interface_ = robot_hw->get<franka_hw::FrankaStateInterface>();
  if (franka_state_interface_ == nullptr) {
    ROS_ERROR("EffortJointImpedanceController: Could not get Franka state interface from hardware");
    return false;
  }
  std::vector<std::string> joint_names;
  if (!node_handle.getParam("joint_names", joint_names) || joint_names.size() != 7) {
    ROS_ERROR(
        "EffortJointImpedanceController: Invalid or no joint_names parameters provided, aborting "
        "controller init!");
    return false;
  }

  if (!node_handle.getParam("k_gains", k_gains_) || k_gains_.size() != 7) {
    ROS_ERROR(
        "EffortJointImpedanceController:  Invalid or no k_gain parameters provided, aborting "
        "controller init!");
    return false;
  }

  if (!node_handle.getParam("d_gains", d_gains_) || d_gains_.size() != 7) {
    ROS_ERROR(
        "EffortJointImpedanceController:  Invalid or no d_gain parameters provided, aborting "
        "controller init!");
    return false;
  }

  double controller_state_publish_rate(30.0);
  if (!node_handle.getParam("controller_state_publish_rate", controller_state_publish_rate)) {
    ROS_INFO_STREAM("FrankaStateController: Did not find controller_state_publish_rate. Using default "
                    << controller_state_publish_rate << " [Hz].");
  }
  trigger_publish_ = franka_hw::TriggerRate(controller_state_publish_rate);

  if (!node_handle.getParam("coriolis_factor", coriolis_factor_)) {
    ROS_INFO_STREAM("EffortJointImpedanceController: coriolis_factor not found. Defaulting to "
                    << coriolis_factor_);
  }

  auto* model_interface = robot_hw->get<franka_hw::FrankaModelInterface>();
  if (model_interface == nullptr) {
    ROS_ERROR_STREAM(
        "EffortJointImpedanceController: Error getting model interface from hardware");
    return false;
  }
  try {
    model_handle_ = std::make_unique<franka_hw::FrankaModelHandle>(
        model_interface->getHandle(arm_id + "_model"));
  } catch (hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM(
        "EffortJointImpedanceController: Exception getting model handle from interface: "
        << ex.what());
    return false;
  }

  try {
    franka_state_handle_ = std::make_unique<franka_hw::FrankaStateHandle>(
        franka_state_interface_->getHandle(arm_id + "_robot"));
  } catch (const hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM("EffortJointImpedanceController: Exception getting franka state handle: " << ex.what());
    return false;
  }


  auto* effort_joint_interface = robot_hw->get<hardware_interface::EffortJointInterface>();
  if (effort_joint_interface == nullptr) {
    ROS_ERROR_STREAM(
        "EffortJointImpedanceController: Error getting effort joint interface from hardware");
    return false;
  }
  for (size_t i = 0; i < 7; ++i) {
    try {
      joint_handles_.push_back(effort_joint_interface->getHandle(joint_names[i]));
    } catch (const hardware_interface::HardwareInterfaceException& ex) {
      ROS_ERROR_STREAM(
          "EffortJointImpedanceController: Exception getting joint handles: " << ex.what());
      return false;
    }
  }
  k_gains_target_ = k_gains_;
  d_gains_target_ = d_gains_;

  dynamic_reconfigure_controller_gains_node_ =
      ros::NodeHandle("effort_joint_impedance_controller/arm/controller_parameters_config");

  dynamic_server_controller_config_ = std::make_unique<
      dynamic_reconfigure::Server<franka_ros_controllers::joint_position_controller_paramsConfig>>(

      dynamic_reconfigure_controller_gains_node_);
  dynamic_server_controller_config_->setCallback(
      boost::bind(&EffortJointImpedanceController::controllerConfigCallback, this, _1, _2));

  desired_joints_subscriber_ = node_handle.subscribe(
      "arm/joint_commands", 20, &EffortJointImpedanceController::jointCmdCallback, this,
      ros::TransportHints().reliable().tcpNoDelay());

  publisher_controller_states_.init(node_handle, "arm/joint_controller_states", 1);

  {
    std::lock_guard<realtime_tools::RealtimePublisher<franka_core_msgs::JointControllerStates> > lock(
        publisher_controller_states_);
    publisher_controller_states_.msg_.names.resize(joint_names.size());
    publisher_controller_states_.msg_.joint_controller_states.resize(joint_names.size());

  }

  std::fill(dq_filtered_.begin(), dq_filtered_.end(), 0);

  return true;
}

void EffortJointImpedanceController::starting(const ros::Time& /*time*/) {
  franka::RobotState robot_state = franka_state_handle_->getRobotState();
  for (size_t i = 0; i < 7; ++i) {
    initial_pos_[i] = robot_state.q[i];
    std::cout << "Joint Pos: " << initial_pos_[i]  << std::endl;
    // initial_vel_[i] = robot_state.dq[i];
  }
  prev_pos_ = initial_pos_;
  pos_d_target_ = initial_pos_;

  std::fill(dq_filtered_.begin(), dq_filtered_.end(), 0);
  dq_d_ = dq_filtered_;
}

void EffortJointImpedanceController::update(const ros::Time& time,
                                             const ros::Duration& period) {
  franka::RobotState robot_state = franka_state_handle_->getRobotState();
  std::array<double, 7> coriolis = model_handle_->getCoriolis();
  std::array<double, 7> gravity = model_handle_->getGravity();

  double alpha = 0.99;
  for (size_t i = 0; i < 7; i++) {
    dq_filtered_[i] = (1 - alpha) * dq_filtered_[i] + alpha * robot_state.dq[i];
  }

  std::array<double, 7> tau_d_calculated{};
  for (size_t i = 0; i < 7; ++i) {
    tau_d_calculated[i] = coriolis_factor_ * coriolis[i] +
                          k_gains_[i] * (pos_d_target_[i] - robot_state.q[i]) +
                          d_gains_[i] * (dq_d_[i] - dq_filtered_[i]);
  }

  // Maximum torque difference with a sampling rate of 1 kHz. The maximum torque rate is
  // 1000 * (1 / sampling_time).
  std::array<double, 7> tau_d_saturated = saturateTorqueRate(tau_d_calculated);

  if (trigger_publish_() && publisher_controller_states_.trylock()) {
      for (size_t i = 0; i < 7; ++i){

        publisher_controller_states_.msg_.joint_controller_states[i].set_point = pos_d_target_[i];
        publisher_controller_states_.msg_.joint_controller_states[i].process_value = robot_state.q[i];
        publisher_controller_states_.msg_.joint_controller_states[i].process_value_dot = robot_state.dq[i];
        publisher_controller_states_.msg_.joint_controller_states[i].error = pos_d_target_[i] - robot_state.q[i];
        publisher_controller_states_.msg_.joint_controller_states[i].time_step = period.toSec();
        publisher_controller_states_.msg_.joint_controller_states[i].command = tau_d_calculated[i];

        publisher_controller_states_.msg_.joint_controller_states[i].p = k_gains_[i];
        publisher_controller_states_.msg_.joint_controller_states[i].d = d_gains_[i];
        publisher_controller_states_.msg_.joint_controller_states[i].header.stamp = time;

      }

      publisher_controller_states_.unlockAndPublish();

    }

  for (size_t i = 0; i < 7; ++i) {
    joint_handles_[i].setCommand(tau_d_saturated[i]);

    k_gains_[i] = filter_params_ * k_gains_target_[i] + (1.0 - filter_params_) * k_gains_[i];
    d_gains_[i] = filter_params_ * d_gains_target_[i] + (1.0 - filter_params_) * d_gains_[i];
  }

}

std::array<double, 7> EffortJointImpedanceController::saturateTorqueRate(
    const std::array<double, 7>& tau_d_calculated) {  // NOLINT (readability-identifier-naming)
  std::array<double, 7> tau_d_saturated{};
  for (size_t i = 0; i < 7; i++) {
    tau_d_saturated[i] = std::max(std::min(tau_d_calculated[i], kDeltaTauMax), -kDeltaTauMax);
  }
  return tau_d_saturated;
}

void EffortJointImpedanceController::jointCmdCallback(const franka_core_msgs::JointCommandConstPtr& msg) {

  if (msg->mode == franka_core_msgs::JointCommand::TORQUE_MODE){
    if (msg->position.size() != 7) {
      ROS_ERROR_STREAM(
          "EffortJointImpedanceController: Published Commands are not of size 7");
      pos_d_target_ = prev_pos_;
    }
    else {
      std::copy_n(msg->position.begin(), 7, pos_d_target_.begin());
      std::copy_n(msg->velocity.begin(), 7, dq_d_.begin()); // if velocity is not there, the controller fails!!
      // std::cout << "Desired Joint Pos: " << pos_d_target_[0] << "  " << pos_d_target_[2] << std::endl;
    }
  }
  else ROS_ERROR_STREAM("EffortJointImpedanceController: Published Command msg are not it JointCommand::TORQUE_MODE! Dropping message");
}

void EffortJointImpedanceController::controllerConfigCallback(
    franka_ros_controllers::joint_position_controller_paramsConfig& config,
    uint32_t /*level*/) {

    k_gains_target_[0] = config.j1_k;
    k_gains_target_[1] = config.j2_k;
    k_gains_target_[2] = config.j3_k;
    k_gains_target_[3] = config.j4_k;
    k_gains_target_[4] = config.j5_k;
    k_gains_target_[5] = config.j6_k;
    k_gains_target_[6] = config.j7_k;

    d_gains_target_[0] = config.j1_d;
    d_gains_target_[1] = config.j2_d;
    d_gains_target_[2] = config.j3_d;
    d_gains_target_[3] = config.j4_d;
    d_gains_target_[4] = config.j5_d;
    d_gains_target_[5] = config.j6_d;
    d_gains_target_[6] = config.j7_d;

}

}  // namespace franka_ros_controllers

PLUGINLIB_EXPORT_CLASS(franka_ros_controllers::EffortJointImpedanceController,
                       controller_interface::ControllerBase)
