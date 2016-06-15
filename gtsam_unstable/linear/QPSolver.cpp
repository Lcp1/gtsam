/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    QPSolver.cpp
 * @brief
 * @date    Apr 15, 2014
 * @author  Duy-Nguyen Ta
 */

#include <gtsam/inference/Symbol.h>
#include <gtsam/inference/FactorGraph-inst.h>
#include <gtsam_unstable/linear/QPSolver.h>
#include <gtsam_unstable/linear/LPSolver.h>
#include <gtsam_unstable/linear/InfeasibleInitialValues.h>
#include <boost/range/adaptor/map.hpp>
#include <gtsam_unstable/linear/LPInitSolver.h>

using namespace std;

namespace gtsam {

//******************************************************************************
QPSolver::QPSolver(const QP& qp) :
    qp_(qp) {
  baseGraph_ = qp_.cost;
  baseGraph_.push_back(qp_.equalities.begin(), qp_.equalities.end());
  costVariableIndex_ = VariableIndex(qp_.cost);
  equalityVariableIndex_ = VariableIndex(qp_.equalities);
  inequalityVariableIndex_ = VariableIndex(qp_.inequalities);
  constrainedKeys_ = qp_.equalities.keys();
  constrainedKeys_.merge(qp_.inequalities.keys());
}

//***************************************************cc***************************
VectorValues QPSolver::solveWithCurrentWorkingSet(
    const InequalityFactorGraph& workingSet) const {
  GaussianFactorGraph workingGraph = baseGraph_;
  for (const LinearInequality::shared_ptr& factor : workingSet) {
    if (factor->active())
      workingGraph.push_back(factor);
  }
  return workingGraph.optimize();
}

//******************************************************************************
JacobianFactor::shared_ptr QPSolver::createDualFactor(Key key,
    const InequalityFactorGraph& workingSet, const VectorValues& delta) const {
  // Transpose the A matrix of constrained factors to have the jacobian of the
  // dual key
  std::vector < std::pair<Key, Matrix> > Aterms = collectDualJacobians
      < LinearEquality > (key, qp_.equalities, equalityVariableIndex_);
  std::vector < std::pair<Key, Matrix> > AtermsInequalities =
      collectDualJacobians < LinearInequality
          > (key, workingSet, inequalityVariableIndex_);
  Aterms.insert(Aterms.end(), AtermsInequalities.begin(),
      AtermsInequalities.end());

  // Collect the gradients of unconstrained cost factors to the b vector
  if (Aterms.size() > 0) {
    Vector b = Vector::Zero(delta.at(key).size());
    if (costVariableIndex_.find(key) != costVariableIndex_.end()) {
      for (size_t factorIx : costVariableIndex_[key]) {
        GaussianFactor::shared_ptr factor = qp_.cost.at(factorIx);
        b += factor->gradient(key, delta);
      }
    }
    return boost::make_shared < JacobianFactor > (Aterms, b); // compute the least-square approximation of dual variables
  } else {
    return boost::make_shared<JacobianFactor>();
  }
}

//******************************************************************************
boost::tuple<double, int> QPSolver::computeStepSize(
    const InequalityFactorGraph& workingSet, const VectorValues& xk,
    const VectorValues& p) const {
  return ActiveSetSolver::computeStepSize(workingSet, xk, p, 1);
}

//******************************************************************************
QPState QPSolver::iterate(const QPState& state) const {
  // Algorithm 16.3 from Nocedal06book.
  // Solve with the current working set eqn 16.39, but instead of solving for p
  // solve for x
  VectorValues newValues = solveWithCurrentWorkingSet(state.workingSet);
  // If we CAN'T move further
  // if p_k = 0 is the original condition, modified by Duy to say that the state
  // update is zero.
  if (newValues.equals(state.values, 1e-7)) {
    // Compute lambda from the dual graph
    GaussianFactorGraph::shared_ptr dualGraph = buildDualGraph(state.workingSet,
        newValues);
    VectorValues duals = dualGraph->optimize();
    int leavingFactor = identifyLeavingConstraint(state.workingSet, duals);
    // If all inequality constraints are satisfied: We have the solution!!
    if (leavingFactor < 0) {
      return QPState(newValues, duals, state.workingSet, true,
          state.iterations + 1);
    } else {
      // Inactivate the leaving constraint
      InequalityFactorGraph newWorkingSet = state.workingSet;
      newWorkingSet.at(leavingFactor)->inactivate();
      return QPState(newValues, duals, newWorkingSet, false,
          state.iterations + 1);
    }
  } else {
    // If we CAN make some progress, i.e. p_k != 0
    // Adapt stepsize if some inactive constraints complain about this move
    double alpha;
    int factorIx;
    VectorValues p = newValues - state.values;
    boost::tie(alpha, factorIx) = // using 16.41
        computeStepSize(state.workingSet, state.values, p);
    // also add to the working set the one that complains the most
    InequalityFactorGraph newWorkingSet = state.workingSet;
    if (factorIx >= 0)
      newWorkingSet.at(factorIx)->activate();
    // step!
    newValues = state.values + alpha * p;
    return QPState(newValues, state.duals, newWorkingSet, false,
        state.iterations + 1);
  }
}

//******************************************************************************
InequalityFactorGraph QPSolver::identifyActiveConstraints(
    const InequalityFactorGraph& inequalities,
    const VectorValues& initialValues, const VectorValues& duals,
    bool useWarmStart) const {
  InequalityFactorGraph workingSet;
  for (const LinearInequality::shared_ptr& factor : inequalities) {
    LinearInequality::shared_ptr workingFactor(new LinearInequality(*factor));
    if (useWarmStart == true && duals.exists(workingFactor->dualKey())) {
      workingFactor->activate();
    } else {
      if (useWarmStart == true && duals.size() > 0) {
        workingFactor->inactivate();
      } else {
        double error = workingFactor->error(initialValues);
        // TODO: find a feasible initial point for QPSolver.
        // For now, we just throw an exception, since we don't have an LPSolver
        // to do this yet
        if (error > 0)
          throw InfeasibleInitialValues();

        if (fabs(error) < 1e-7) {
          workingFactor->activate();
        } else {
          workingFactor->inactivate();
        }
      }
    }
    workingSet.push_back(workingFactor);
  }
  return workingSet;
}

//******************************************************************************
pair<VectorValues, VectorValues> QPSolver::optimize(
    const VectorValues& initialValues, const VectorValues& duals,
    bool useWarmStart) const {
  // Initialize workingSet from the feasible initialValues
  InequalityFactorGraph workingSet = identifyActiveConstraints(qp_.inequalities,
      initialValues, duals, useWarmStart);
  QPState state(initialValues, duals, workingSet, false, 0);

  /// main loop of the solver
  while (!state.converged)
    state = iterate(state);

  return make_pair(state.values, state.duals);
}

pair<VectorValues, VectorValues> QPSolver::optimize() const {
  //Make an LP with any linear cost function. It doesn't matter for initialization.
  LP initProblem;
  // make an unrelated key for a random variable cost: max key + 1
  Key newKey = *qp_.cost.keys().rbegin();
  if (!qp_.equalities.empty())
    newKey = std::max(newKey, *qp_.equalities.keys().rbegin());
  if (!qp_.inequalities.empty())
    newKey = std::max(newKey, *qp_.inequalities.keys().rbegin());
  ++newKey;
  initProblem.cost = LinearCost(newKey, Vector::Ones(1));
  initProblem.equalities = qp_.equalities;
  initProblem.inequalities = qp_.inequalities;
  LPSolver solver(initProblem);
  LPInitSolver initSolver(solver);
  VectorValues initValues = initSolver.solve();

  return optimize(initValues);
}
;

} /* namespace gtsam */
