/**
 * actions.h
 *
 * Copyright 2018. All Rights Reserved.
 *
 * Created: November 28, 2018
 * Authors: Toki Migimatsu
 */

#ifndef LOGIC_OPT_PLANNING_ACTIONS_H_
#define LOGIC_OPT_PLANNING_ACTIONS_H_

#include <memory>  // std::shared_ptr
#include <vector>  // std::vector
#include <set>     // std::set

#include "ptree.h"

#include "logic_opt/planning/formula.h"
#include "logic_opt/planning/objects.h"
#include "logic_opt/planning/proposition.h"

namespace logic_opt {

std::vector<const VAL::parameter_symbol*> FilterEffectArgs(const std::vector<const VAL::parameter_symbol*>& action_args,
                                                           const VAL::var_symbol_list* action_params,
                                                           const VAL::parameter_symbol_list* effect_params);

/**
 * Apply action postconditions.
 */
std::set<Proposition> ApplyEffects(FormulaMap& formulas,
                                   const std::shared_ptr<const ObjectTypeMap>& objects,
                                   const std::vector<const VAL::parameter_symbol*>& action_args,
                                   const VAL::var_symbol_list* action_params,
                                   const VAL::effect_lists* effects,
                                   const std::set<Proposition>& propositions);

class Action {

 public:

  Action(const VAL::operator_* op) : symbol_(op) {}

  Action(const VAL::domain* domain, const std::string& name_action);

  const VAL::operator_* symbol() const { return symbol_; }

 private:

  const VAL::operator_* symbol_;

};

}  // namespace logic_opt

#endif  // LOGIC_OPT_PLANNING_ACTIONS_H_
