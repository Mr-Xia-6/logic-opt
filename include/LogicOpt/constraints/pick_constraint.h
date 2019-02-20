/**
 * pick_constraint.h
 *
 * Copyright 2018. All Rights Reserved.
 *
 * Created: December 13, 2018
 * Authors: Toki Migimatsu
 */

#ifndef LOGIC_OPT_PICK_CONSTRAINT_H_
#define LOGIC_OPT_PICK_CONSTRAINT_H_

#include "LogicOpt/constraints/constraint.h"

namespace LogicOpt {

class PickConstraint : virtual public FrameConstraint {

 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW;

  PickConstraint(World& world, size_t t_pick, const std::string& name_ee,
                 const std::string& name_object);

  virtual ~PickConstraint() = default;

  virtual void Evaluate(Eigen::Ref<const Eigen::MatrixXd> X,
                        Eigen::Ref<Eigen::VectorXd> constraints) override;

  virtual void Jacobian(Eigen::Ref<const Eigen::MatrixXd> X,
                        Eigen::Ref<Eigen::VectorXd> Jacobian) override;

 protected:

  virtual Eigen::Vector3d ComputeError(Eigen::Ref<const Eigen::MatrixXd> X) const;

  Eigen::Vector3d x_err_ = Eigen::Vector3d::Zero();

  const World& world_;

};

}  // namespace LogicOpt

#endif  // LOGIC_OPT_PICK_CONSTRAINT_H_
