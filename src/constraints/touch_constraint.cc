/**
 * touch_constraint.cc
 *
 * Copyright 2019. All Rights Reserved.
 *
 * Created: February 20, 2019
 * Authors: Toki Migimatsu
 */

#include "logic_opt/constraints/touch_constraint.h"

#include <cmath>      // std::abs
#include <exception>  // std::runtime_error
#include <sstream>    // std::stringstream

namespace {

const double kH = 1e-2;
const double kEpsilon = 1e-6;
const double kMaxDist = 100.;

}

namespace logic_opt {

TouchConstraint::TouchConstraint(World3& world, size_t t_touch,
                                 const std::string& name_control, const std::string& name_target)
    : FrameConstraint(kNumConstraints, kLenJacobian, t_touch, kNumTimesteps, name_control, name_target,
                      "constraint_t" + std::to_string(t_touch) + "_touch"),
      world_(world) {
  if (name_control == world.kWorldFrame) {
    throw std::invalid_argument("TouchConstraint::TouchConstraint(): " + world.kWorldFrame +
                                " cannot be the control frame.");
  } else if (name_target == world.kWorldFrame) {
    throw std::invalid_argument("TouchConstraint::TouchConstraint(): " + world.kWorldFrame +
                                " cannot be the target frame.");
  }
  world.ReserveTimesteps(t_touch + kNumTimesteps);
  world.AttachFrame(name_control, name_target, t_touch);
}

void TouchConstraint::Evaluate(Eigen::Ref<const Eigen::MatrixXd> X,
                               Eigen::Ref<Eigen::VectorXd> constraints) {
  contact_ = ComputeError(X);             // signed_dist
  if (!contact_) {
    std::cerr << name << "::Evaluate(): No contact!" << std::endl;
    std::cerr << X.col(t_start()).transpose() << std::endl;
    Constraint::Evaluate(X, constraints);
    return;
  }

  constraints(0) = -0.5 * std::abs(contact_->depth) * contact_->depth;   // signed_dist < epsilon

  Constraint::Evaluate(X, constraints);
}

void TouchConstraint::Jacobian(Eigen::Ref<const Eigen::MatrixXd> X,
                               Eigen::Ref<Eigen::VectorXd> Jacobian) {
  if (!contact_) {
    Constraint::Jacobian(X, Jacobian);
    return;
  }
  Jacobian.head<3>() = std::abs(contact_->depth) * contact_->normal;
  const double x_err = -contact_->depth;

  if (contact_->depth == 0.) throw std::runtime_error(name + "::Jacobian(): 0 depth.");

  Eigen::MatrixXd X_h = X;
  for (size_t i = 3; i < kDof; i++) {
    double& x_it = X_h(i, t_start());
    const double x_it_0 = x_it;
    x_it = x_it_0 + kH;
    const auto contact_hp = ComputeError(X_h);
    const double x_err_hp = contact_hp ? -contact_hp->depth : 0.;
    if (contact_hp->depth == 0.) throw std::runtime_error(name + "::Jacobian(): 0 depth hp.");
    x_it = x_it_0 - kH;
    const auto contact_hn = ComputeError(X_h);
    const double x_err_hn = contact_hn ? -contact_hn->depth : 0.;
    if (contact_hn->depth == 0.) throw std::runtime_error(name + "::Jacobian(): 0 depth hn.");
    x_it = x_it_0;

    const double dx_h = 0.5 * (std::abs(x_err_hp) * x_err_hp - std::abs(x_err_hn) * x_err_hn) / (2. * kH);

    if (contact_->depth < 0 && std::abs(dx_h) > 0.5) {
      // If the contact distance is small, ncollide will clip it to 0, which
      // will make either x_err_hp or x_err_hn 0, and dx_h will become huge. Get
      // around this by leaving the Jacobian element 0.

      std::stringstream ss;
      ss << "TouchConstraint::Jacobian(): Ill-conditioned J(" << i << ","
         << t_start() << "): " << dx_h << " " << contact_hn.has_value() << " " << x_err_hp << " " << x_err_hn << " " << x_err << std::endl;
      throw std::runtime_error(ss.str());
    }

    Jacobian(i) = dx_h;
  }
  Constraint::Jacobian(X, Jacobian);
}

void TouchConstraint::JacobianIndices(Eigen::Ref<Eigen::ArrayXi> idx_i,
                                      Eigen::Ref<Eigen::ArrayXi> idx_j) {
  // i:  0  0  0  0  0  0
  // j: px py pz wx wy wz
  const size_t var_t = kDof * t_start();
  idx_j.head<kDof>().setLinSpaced(var_t, var_t + kDof - 1);
}

std::optional<ncollide3d::query::Contact>
TouchConstraint::ComputeError(Eigen::Ref<const Eigen::MatrixXd> X) const {
  const Object3& control = world_.objects()->at(control_frame());
  const Object3& target = world_.objects()->at(target_frame());
  const Eigen::Isometry3d T_control_to_target = world_.T_control_to_target(X, t_start());

  // Signed distance: positive if objects overlap
  return ncollide3d::query::contact(Eigen::Isometry3d::Identity(), *target.collision,
                                    T_control_to_target, *control.collision, kMaxDist);
}

}  // namespace logic_opt
