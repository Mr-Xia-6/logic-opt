/**
 * constraints.cc
 *
 * Copyright 2018. All Rights Reserved.
 *
 * Created: October 27, 2018
 * Authors: Toki Migimatsu
 */

#include "constraints.h"

#include <cassert>  // assert

namespace TrajOpt {

void Constraint::Evaluate(Eigen::Ref<const Eigen::MatrixXd> Q,
                          Eigen::Ref<Eigen::VectorXd> constraints) {
  if (!log.is_open()) return;
  log << constraints.transpose() << std::endl;
}

void JointPositionConstraint::Evaluate(Eigen::Ref<const Eigen::MatrixXd> Q,
                                       Eigen::Ref<Eigen::VectorXd> constraints) {
  constraints = 0.5 * (Q.col(t_start) - q_des).array().square();
  Constraint::Evaluate(Q, constraints);
}

void JointPositionConstraint::Jacobian(Eigen::Ref<const Eigen::MatrixXd> Q,
                                       Eigen::Ref<Eigen::VectorXd> Jacobian) {
  Jacobian = Q.col(t_start) - q_des;
}

void JointPositionConstraint::JacobianIndices(Eigen::Ref<Eigen::ArrayXi> idx_i,
                                              Eigen::Ref<Eigen::ArrayXi> idx_j) {
  const size_t& dof = len_jacobian;
  for (size_t i = 0; i < len_jacobian; i++) {
    idx_i(i) += i;
    idx_j(i) = dof * t_start + i;
  }
}

void JointPositionConstraint::Hessian(Eigen::Ref<const Eigen::MatrixXd> Q,
                                      Eigen::Ref<const Eigen::VectorXd> lambda,
                                      Eigen::Ref<Eigen::SparseMatrix<double>> Hessian) {
  const size_t& dof = Q.rows();
  for (size_t i = 0; i < dof; i++) {
    Hessian.coeffRef(t_start * dof + i, t_start * dof + i) += lambda(i);
  }
}

void JointPositionConstraint::HessianStructure(Eigen::SparseMatrix<bool>& Hessian,
                                               size_t T) {
  const size_t& dof = len_jacobian;
  for (size_t i = 0; i < dof; i++) {
    if (Hessian.coeff(t_start * dof + i, t_start * dof + i)) continue;
    Hessian.insert(t_start * dof + i, t_start * dof + i) = true;
  }
}

void CartesianPoseConstraint::Evaluate(Eigen::Ref<const Eigen::MatrixXd> Q,
                                       Eigen::Ref<Eigen::VectorXd> constraints) {
  ComputeError(Q);

  // Position
  switch (layout) {
    case Layout::SCALAR:  // Combined position, orientation scalar
      constraints(0) = 0.5 * x_quat_err_.squaredNorm();
      break;
    case Layout::POS_SCALAR:
    case Layout::SCALAR_SCALAR:
    case Layout::SCALAR_VECTOR:  // Position scalar
      constraints(0) = 0.5 * x_quat_err_.head<3>().squaredNorm();
      break;
    case Layout::POS_VECTOR:
    case Layout::VECTOR_SCALAR:
    case Layout::VECTOR_VECTOR:  // Position vector
      constraints.head(3) = 0.5 * x_quat_err_.head<3>().array().square();
      break;
    default: break;
  }

  // Orientation
  switch (layout) {
    case Layout::ORI_SCALAR:
    case Layout::SCALAR_SCALAR:
    case Layout::VECTOR_SCALAR:  // Orientation scalar
      constraints(constraints.size() - 1) = 0.5 * x_quat_err_.tail<3>().squaredNorm();
      break;
    case Layout::ORI_VECTOR:
    case Layout::SCALAR_VECTOR:
    case Layout::VECTOR_VECTOR:  // Orientation vector
      constraints.tail(3) = 0.5 * x_quat_err_.tail<3>().array().square();
      break;
    default: break;
  }
  Constraint::Evaluate(Q, constraints);
}

void CartesianPoseConstraint::Jacobian(Eigen::Ref<const Eigen::MatrixXd> Q,
                                       Eigen::Ref<Eigen::VectorXd> Jacobian) {
  Eigen::Map<Eigen::MatrixXd> J(&Jacobian(0), num_constraints, ab_.dof());

  ComputeError(Q);
  const Eigen::Matrix6Xd& J_x = SpatialDyn::Jacobian(ab_, -1, ee_offset);

  // Position
  switch (layout) {
    case Layout::SCALAR:  // Combined position, orientation scalar
      J = x_quat_err_.transpose() * J_x;
      break;
    case Layout::POS_SCALAR:
    case Layout::SCALAR_SCALAR:
    case Layout::SCALAR_VECTOR:  // Position scalar
      J.row(0) = x_quat_err_.head<3>().transpose() * J_x.topRows<3>();
      break;
    case Layout::POS_VECTOR:
    case Layout::VECTOR_SCALAR:
    case Layout::VECTOR_VECTOR:  // Position vector
      J.topRows(3) = J_x.topRows<3>().array().colwise() * x_quat_err_.head<3>().array();
      break;
    default: break;
  }

  // Orientation
  switch (layout) {
    case Layout::ORI_SCALAR:
    case Layout::SCALAR_SCALAR:
    case Layout::VECTOR_SCALAR:  // Orientation scalar
      J.row(J.rows() - 1) = x_quat_err_.tail<3>().transpose() * J_x.bottomRows<3>();
      break;
    case Layout::ORI_VECTOR:
    case Layout::SCALAR_VECTOR:
    case Layout::VECTOR_VECTOR:  // Orientation vector
      J.bottomRows(3) = J_x.bottomRows<3>().array().colwise() * x_quat_err_.tail<3>().array();
      break;
    default: break;
  }
}

void CartesianPoseConstraint::ComputeError(Eigen::Ref<const Eigen::MatrixXd> Q) {
  if (q_.size() == ab_.dof() && (q_.array() == Q.col(t_start).array()).all()) return;

  ab_.set_q(Q.col(t_start));
  if (layout != Layout::ORI_SCALAR && layout != Layout::ORI_VECTOR) {
    x_quat_err_.head<3>() = SpatialDyn::Position(ab_, -1, ee_offset) - x_des;
  }
  if (layout != Layout::POS_SCALAR && layout != Layout::POS_VECTOR) {
    x_quat_err_.tail<3>() = SpatialDyn::Opspace::OrientationError(SpatialDyn::Orientation(ab_), quat_des);
  }
  q_ = ab_.q();
}

void CartesianPoseConstraint::JacobianIndices(Eigen::Ref<Eigen::ArrayXi> idx_i,
                                              Eigen::Ref<Eigen::ArrayXi> idx_j) {
  Eigen::Map<Eigen::MatrixXi> Idx_i(&idx_i(0), num_constraints, ab_.dof());
  Eigen::Map<Eigen::MatrixXi> Idx_j(&idx_j(0), num_constraints, ab_.dof());

  Idx_i.colwise() += Eigen::VectorXi::LinSpaced(num_constraints, 0, num_constraints - 1);
  Idx_j.rowwise() = Eigen::VectorXi::LinSpaced(ab_.dof(), t_start * ab_.dof(),
                                               (t_start + 1) * ab_.dof() - 1).transpose();
}

void CartesianPoseConstraint::Hessian(Eigen::Ref<const Eigen::MatrixXd> Q,
                                      Eigen::Ref<const Eigen::VectorXd> lambda,
                                      Eigen::Ref<Eigen::SparseMatrix<double>> Hessian) {
  const size_t& dof = Q.rows();
  Eigen::MatrixXd H(dof, dof);
  Eigen::TensorMap<Eigen::Tensor2d> tensor_H(&H(0, 0), dof, dof);
  Eigen::Vector6d lambda_6d = Eigen::Vector6d::Zero();

  // Position
  switch (layout) {
    case Layout::SCALAR:  // Combined position, orientation scalar
      lambda_6d.fill(lambda(0));
      break;
    case Layout::POS_SCALAR:
    case Layout::SCALAR_SCALAR:
    case Layout::SCALAR_VECTOR:  // Position scalar
      lambda_6d.head<3>().fill(lambda(0));
      break;
    case Layout::POS_VECTOR:
    case Layout::VECTOR_SCALAR:
    case Layout::VECTOR_VECTOR:  // Position vector
      lambda_6d.head<3>() = lambda.head(3);
      break;
    default: break;
  }

  // Orientation
  switch (layout) {
    case Layout::ORI_SCALAR:
    case Layout::SCALAR_SCALAR:
    case Layout::VECTOR_SCALAR:  // Orientation scalar
      lambda_6d.tail<3>().fill(lambda(lambda.size() - 1));
      break;
    case Layout::ORI_VECTOR:
    case Layout::SCALAR_VECTOR:
    case Layout::VECTOR_VECTOR:  // Orientation vector
      lambda_6d.tail<3>() = lambda.tail(3);
      break;
    default: break;
  }

  ComputeError(Q);
  const Eigen::Matrix6Xd& J = SpatialDyn::Jacobian(ab_, -1, ee_offset);
  H = J.transpose() * (lambda_6d.asDiagonal() * J);

  Eigen::Vector6d dx = lambda_6d.array() * x_quat_err_.array();
  Eigen::TensorMap<Eigen::Tensor1d> tensor_dx(&dx(0), 6);

  Eigen::array<Eigen::IndexPair<int>, 1> product_dims = { Eigen::IndexPair<int>(2, 0) };
  Eigen::Tensor2d tH = SpatialDyn::Hessian(ab_).contract(tensor_dx, product_dims);
  // std::cout << "H JTJ2: " << std::endl << tensor_H << std::endl << std::endl;
  // std::cout << "H dJ: " << std::endl << tH << std::endl << std::endl;
  // std::cout << "dx: " << dx.transpose() << std::endl;
  tensor_H += SpatialDyn::Hessian(ab_, -1, ee_offset).contract(tensor_dx, product_dims);

  for (size_t j = 0; j < dof; j++) {
    for (size_t i = 0; i <= j; i++) {
      Hessian.coeffRef(i + t_start * dof, j + t_start * dof) += H(i, j);
    }
  }
}

void CartesianPoseConstraint::HessianStructure(Eigen::SparseMatrix<bool>& Hessian,
                                               size_t T) {
  for (size_t j = 0; j < ab_.dof(); j++) {
    for (size_t i = 0; i <= j; i++) {
      if (Hessian.coeff(i + t_start * ab_.dof(), j + t_start * ab_.dof())) continue;
      Hessian.insert(i + t_start * ab_.dof(), j + t_start * ab_.dof()) = true;
    }
  }
}

size_t CartesianPoseConstraint::NumConstraints(CartesianPoseConstraint::Layout l) {
  switch (l) {
    case Layout::SCALAR:
    case Layout::POS_SCALAR:
    case Layout::ORI_SCALAR:
      return 1;
    case Layout::SCALAR_SCALAR:
      return 2;
    case Layout::POS_VECTOR:
    case Layout::ORI_VECTOR:
      return 3;
    case Layout::VECTOR_SCALAR:
    case Layout::SCALAR_VECTOR:
      return 4;
    case Layout::VECTOR_VECTOR: return 6;
  }
}

PickConstraint::PickConstraint(const World& world, size_t t_pick, const std::string& name_object,
                               const Eigen::Vector3d& ee_offset, Layout layout)
    : Constraint(NumConstraints(layout), NumConstraints(layout) * world.ab.dof(), t_pick, 1,
                 Type::EQUALITY, "constraint_pick_t" + std::to_string(t_pick)),
      CartesianPoseConstraint(world.ab, t_pick, Eigen::Vector3d::Zero(),
                              Eigen::Quaterniond::Identity(), ee_offset, layout),
      world_(world), name_object(name_object) {}

void PickConstraint::Evaluate(Eigen::Ref<const Eigen::MatrixXd> Q,
                              Eigen::Ref<Eigen::VectorXd> constraints) {
  x_des = world_.objects.at(name_object).T_to_parent().translation();
  CartesianPoseConstraint::Evaluate(Q, constraints);
}

void PickConstraint::Jacobian(Eigen::Ref<const Eigen::MatrixXd> Q,
                              Eigen::Ref<Eigen::VectorXd> Jacobian) {
  x_des = world_.objects.at(name_object).T_to_parent().translation();
  CartesianPoseConstraint::Jacobian(Q, Jacobian);
}

void PickConstraint::Hessian(Eigen::Ref<const Eigen::MatrixXd> Q,
                             Eigen::Ref<const Eigen::VectorXd> lambda,
                             Eigen::Ref<Eigen::SparseMatrix<double>> Hessian) {
  x_des = world_.objects.at(name_object).T_to_parent().translation();
  CartesianPoseConstraint::Hessian(Q, lambda, Hessian);
}

void PickConstraint::Simulate(World& world, Eigen::Ref<const Eigen::MatrixXd> Q) {
  Eigen::Isometry3d T_pick_ee_to_world = SpatialDyn::CartesianPose(world.ab, Q.col(t_start));
  const World::ObjectState& object_state = world.object_state(name_object, t_start);

  Eigen::Isometry3d T_pick_object_to_world = Eigen::Translation3d(object_state.pos) * object_state.quat;
  Eigen::Isometry3d T_object_to_ee = T_pick_ee_to_world.inverse() * T_pick_object_to_world;

  for (size_t t = t_start + 1; t < world.T; t++) {
    World::ObjectState& object_state = world.object_state(name_object, t);
    if (object_state.owner != this) break;

    Eigen::Isometry3d T_object_to_world = SpatialDyn::CartesianPose(world.ab, Q.col(t)) * T_object_to_ee;
    object_state.pos = T_object_to_world.translation();
    object_state.quat = Eigen::Quaterniond(T_object_to_world.linear());
  }
}

void PickConstraint::RegisterSimulationStates(World& world) {
  for (size_t t = t_start; t < world.T; t++) {
    World::ObjectState& object_state = world.object_state(name_object, t);
    object_state.owner = this;
  }
}

void PickConstraint::InterpolateSimulation(const World& world,
                                           Eigen::Ref<const Eigen::VectorXd> q, double t,
                                           std::map<std::string, World::ObjectState>& object_states) const {

  Eigen::Isometry3d T_pick_ee_to_world = SpatialDyn::CartesianPose(world.ab, world.Q().col(t_start));
  const World::ObjectState& object_state = world.object_state(name_object, t_start);

  Eigen::Isometry3d T_pick_object_to_world = Eigen::Translation3d(object_state.pos) * object_state.quat;
  Eigen::Isometry3d T_object_to_ee = T_pick_ee_to_world.inverse() * T_pick_object_to_world;

  World::ObjectState& object_state_t = object_states[name_object];
  Eigen::Isometry3d T_object_to_world = SpatialDyn::CartesianPose(world.ab, q) * T_object_to_ee;
  object_state_t.pos = T_object_to_world.translation();
  object_state_t.quat = Eigen::Quaterniond(T_object_to_world.linear());
}

PlaceConstraint::PlaceConstraint(World& world, size_t t_place, const std::string& name_object,
                                 const Eigen::Vector3d& x_des, const Eigen::Quaterniond& quat_des,
                                 const Eigen::Vector3d& ee_offset, Layout layout)
      : Constraint(NumConstraints(layout), NumConstraints(layout) * world.ab.dof(), t_place, 1,
                   Type::EQUALITY, "constraint_place_t" + std::to_string(t_place)),
        CartesianPoseConstraint(world.ab, t_place, x_des, quat_des, ee_offset, layout),
        x_des_place(x_des), quat_des_place(quat_des), name_object(name_object), world_(world) {}

void PlaceConstraint::Evaluate(Eigen::Ref<const Eigen::MatrixXd> Q,
                               Eigen::Ref<Eigen::VectorXd> constraints) {
  ComputePlacePose(Q);
  CartesianPoseConstraint::Evaluate(Q, constraints);
}

void PlaceConstraint::Jacobian(Eigen::Ref<const Eigen::MatrixXd> Q,
                               Eigen::Ref<Eigen::VectorXd> Jacobian) {
  ComputePlacePose(Q);
  CartesianPoseConstraint::Jacobian(Q, Jacobian);
}

void PlaceConstraint::Hessian(Eigen::Ref<const Eigen::MatrixXd> Q,
                              Eigen::Ref<const Eigen::VectorXd> lambda,
                              Eigen::Ref<Eigen::SparseMatrix<double>> Hessian) {
  ComputePlacePose(Q);
  CartesianPoseConstraint::Hessian(Q, lambda, Hessian);
}

void PlaceConstraint::RegisterSimulationStates(World& world) {
  for (size_t t = t_start; t < world.T; t++) {
    World::ObjectState& object_state = world.object_state(name_object, t);
    object_state.pos = x_des_place;
    object_state.quat = quat_des_place;
    object_state.owner = this;
  }
}

void PlaceConstraint::InterpolateSimulation(const World& world,
                                            Eigen::Ref<const Eigen::VectorXd> q, double t,
                                            std::map<std::string, World::ObjectState>& object_states) const {

  Eigen::Isometry3d T_pick_ee_to_world = SpatialDyn::CartesianPose(world.ab, world.Q().col(t_start - 1));
  const World::ObjectState& object_state = world.object_state(name_object, t_start - 1);

  Eigen::Isometry3d T_pick_object_to_world = Eigen::Translation3d(object_state.pos) * object_state.quat;
  Eigen::Isometry3d T_object_to_ee = T_pick_ee_to_world.inverse() * T_pick_object_to_world;

  World::ObjectState& object_state_t = object_states[name_object];
  Eigen::Isometry3d T_object_to_world = SpatialDyn::CartesianPose(world.ab, world.Q().col(t_start)) * T_object_to_ee;
  object_state_t.pos = T_object_to_world.translation();
  object_state_t.quat = Eigen::Quaterniond(T_object_to_world.linear());
}

void PlaceConstraint::ComputePlacePose(Eigen::Ref<const Eigen::MatrixXd> Q) {
  world_.Simulate(Q);
  const World::ObjectState& object_state_prev = world_.object_state(name_object, t_start - 1);
  Eigen::Isometry3d T_object_to_world = Eigen::Translation3d(object_state_prev.pos) * object_state_prev.quat;
  Eigen::Isometry3d T_ee_to_object = T_object_to_world.inverse() *
                                     SpatialDyn::CartesianPose(world_.ab, Q.col(t_start - 1));

  x_des = x_des_place + T_ee_to_object.translation();
  quat_des = T_ee_to_object.linear() * quat_des_place;

  ab_.set_q(Q.col(t_start));
}

}  // namespace TrajOpt
