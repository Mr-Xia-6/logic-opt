/**
 * cartesian_pose_constraint.cc
 *
 * Copyright 2018. All Rights Reserved.
 *
 * Created: November 18, 2018
 * Authors: Toki Migimatsu
 */

#include "logic_opt/constraints/cartesian_pose_constraint.h"

namespace {

const size_t kNumConstraints = 6;
const size_t kLenJacobian = kNumConstraints;
const size_t kNumTimesteps = 1;

}  // namespace

namespace logic_opt {

CartesianPoseConstraint::CartesianPoseConstraint(World3& world, size_t t_goal,
                                                 const std::string& control_frame,
                                                 const std::string& target_frame,
                                                 const Eigen::Vector6d& dx_des)
    : FrameConstraint(kNumConstraints, kLenJacobian, t_goal, kNumTimesteps,
                      control_frame, target_frame,
                      "constraint_cart_pos_t" + std::to_string(t_goal)),
      dx_des_(dx_des) {
  world.ReserveTimesteps(t_goal + kNumTimesteps);
  world.AttachFrame(control_frame_, target_frame_, t_goal);
}

void CartesianPoseConstraint::Evaluate(Eigen::Ref<const Eigen::MatrixXd> X,
                                       Eigen::Ref<Eigen::VectorXd> constraints) {
  ComputeError(X);
  constraints = 0.5 * dx_err_.array().square();
  Constraint::Evaluate(X, constraints);
}

void CartesianPoseConstraint::Jacobian(Eigen::Ref<const Eigen::MatrixXd> X,
                                       Eigen::Ref<Eigen::VectorXd> Jacobian) {
  ComputeError(X);
  Jacobian = dx_err_;
}

void CartesianPoseConstraint::ComputeError(Eigen::Ref<const Eigen::MatrixXd> X) {
  dx_err_ = X.col(t_start_) - dx_des_;
}

}  // namespace logic_opt
