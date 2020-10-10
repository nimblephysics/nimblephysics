#include "dart/trajectory/AbstractShot.hpp"

#include <iostream>

#include <coin/IpIpoptApplication.hpp>
#include <coin/IpReturnCodes.hpp>
#include <coin/IpSolveStatistics.hpp>

#include "dart/neural/IdentityMapping.hpp"
#include "dart/neural/Mapping.hpp"
#include "dart/simulation/World.hpp"

namespace dart {
namespace trajectory {

//==============================================================================
/// Default constructor
AbstractShot::AbstractShot(
    std::shared_ptr<simulation::World> world, LossFn loss, int steps)
  : mWorld(world), mLoss(loss), mSteps(steps), mRolloutCacheDirty(true)
{
  std::shared_ptr<neural::Mapping> identityMapping
      = std::make_shared<neural::IdentityMapping>(world);
  mRepresentationMapping = "identity";
  mMappings[mRepresentationMapping] = identityMapping;
}

//==============================================================================
AbstractShot::~AbstractShot()
{
  // std::cout << "Freeing AbstractShot: " << this << std::endl;
}

//==============================================================================
/// This updates the loss function for this trajectory
void AbstractShot::setLoss(LossFn loss)
{
  mLoss = loss;
}

//==============================================================================
/// Add a custom constraint function to the trajectory
void AbstractShot::addConstraint(LossFn loss)
{
  mConstraints.push_back(loss);
}

/// This sets the mapping we're using to store the representation of the Shot.
/// WARNING: THIS IS A POTENTIALLY DESTRUCTIVE OPERATION! This will rewrite
/// the internal representation of the Shot to use the new mapping, and if the
/// new mapping is underspecified compared to the old mapping, you may lose
/// information. It's not guaranteed that you'll get back the same trajectory
/// if you switch to a different mapping, and then switch back.
///
/// This will affect the values you get back from getStates() - they'll now be
/// returned in the view given by `mapping`. That's also the represenation
/// that'll be passed to IPOPT, and updated on each gradient step. Therein
/// lies the power of changing the representation mapping: There will almost
/// certainly be mapped spaces that are easier to optimize in than native
/// joint space, at least initially.
void AbstractShot::switchRepresentationMapping(
    std::shared_ptr<simulation::World> world, const std::string& mapping)
{
  // Reset the main representation mapping
  mRepresentationMapping = mapping;
  // Clear our cached trajectory
  mRolloutCacheDirty = true;
}

//==============================================================================
/// This adds a mapping through which the loss function can interpret the
/// output. We can have multiple loss mappings at the same time, and loss can
/// use arbitrary combinations of multiple views, as long as it can provide
/// gradients.
void AbstractShot::addMapping(
    const std::string& key, std::shared_ptr<neural::Mapping> mapping)
{
  mMappings[key] = mapping;
  // Clear our cached trajectory
  mRolloutCacheDirty = true;
}

//==============================================================================
/// This returns true if there is a loss mapping at the specified key
bool AbstractShot::hasMapping(const std::string& key)
{
  return mMappings.find(key) != mMappings.end();
}

//==============================================================================
/// This returns the loss mapping at the specified key
std::shared_ptr<neural::Mapping> AbstractShot::getMapping(
    const std::string& key)
{
  return mMappings[key];
}

//==============================================================================
/// This returns a reference to all the mappings in this shot
std::unordered_map<std::string, std::shared_ptr<neural::Mapping>>&
AbstractShot::getMappings()
{
  return mMappings;
}

//==============================================================================
/// This removes the loss mapping at a particular key
void AbstractShot::removeMapping(const std::string& key)
{
  mMappings.erase(key);
  // Clear our cached trajectory
  mRolloutCacheDirty = true;
}

//==============================================================================
/// Returns the sum of posDim() + velDim() for the current representation
/// mapping
int AbstractShot::getRepresentationStateSize() const
{
  return getRepresentation()->getPosDim() + getRepresentation()->getVelDim();
}

//==============================================================================
const std::string& AbstractShot::getRepresentationName() const
{
  return mRepresentationMapping;
}

//==============================================================================
/// Returns the representation currently being used
const std::shared_ptr<neural::Mapping> AbstractShot::getRepresentation() const
{
  return mMappings.at(mRepresentationMapping);
}

//==============================================================================
/// This gets the bounds on the constraint functions (both knot points and any
/// custom constraints)
void AbstractShot::getConstraintUpperBounds(
    /* OUT */ Eigen::Ref<Eigen::VectorXd> flat) const
{
  assert(flat.size() == mConstraints.size());
  for (int i = 0; i < mConstraints.size(); i++)
  {
    flat(i) = mConstraints[i].getUpperBound();
  }
}

//==============================================================================
/// This gets the bounds on the constraint functions (both knot points and any
/// custom constraints)
void AbstractShot::getConstraintLowerBounds(
    /* OUT */ Eigen::Ref<Eigen::VectorXd> flat) const
{
  assert(flat.size() == mConstraints.size());
  for (int i = 0; i < mConstraints.size(); i++)
  {
    flat(i) = mConstraints[i].getLowerBound();
  }
}

//==============================================================================
int AbstractShot::getConstraintDim() const
{
  return mConstraints.size();
}

//==============================================================================
/// This computes the values of the constraints, assuming that the constraint
/// vector being passed in is only the size of mConstraints
void AbstractShot::computeConstraints(
    std::shared_ptr<simulation::World> world,
    /* OUT */ Eigen::Ref<Eigen::VectorXd> constraints)
{
  assert(constraints.size() == mConstraints.size());

  for (int i = 0; i < mConstraints.size(); i++)
  {
    constraints(i) = mConstraints[i].getLoss(getRolloutCache(world));
  }
}

//==============================================================================
/// This computes the Jacobian that relates the flat problem to the end state.
/// This returns a matrix that's (getConstraintDim(), getFlatProblemDim()).
void AbstractShot::backpropJacobian(
    std::shared_ptr<simulation::World> world,
    /* OUT */ Eigen::Ref<Eigen::MatrixXd> jac)
{
  assert(jac.rows() == mConstraints.size());
  assert(jac.cols() == getFlatProblemDim());

  Eigen::VectorXd grad = Eigen::VectorXd::Zero(getFlatProblemDim());
  for (int i = 0; i < mConstraints.size(); i++)
  {
    mConstraints[i].getLossAndGradient(
        getRolloutCache(world),
        /* OUT */ getGradientWrtRolloutCache(world));
    grad.setZero();
    backpropGradientWrt(
        world,
        getGradientWrtRolloutCache(world),
        /* OUT */ grad);
    jac.row(i) = grad;
  }
}

//==============================================================================
/// This gets the number of non-zero entries in the Jacobian
int AbstractShot::getNumberNonZeroJacobian()
{
  return mConstraints.size() * getFlatProblemDim();
}

//==============================================================================
/// This gets the structure of the non-zero entries in the Jacobian
void AbstractShot::getJacobianSparsityStructure(
    Eigen::Ref<Eigen::VectorXi> rows, Eigen::Ref<Eigen::VectorXi> cols)
{
  assert(rows.size() == AbstractShot::getNumberNonZeroJacobian());
  assert(cols.size() == AbstractShot::getNumberNonZeroJacobian());
  int cursor = 0;
  // Do row-major ordering
  for (int j = 0; j < mConstraints.size(); j++)
  {
    for (int i = 0; i < getFlatProblemDim(); i++)
    {
      rows(cursor) = j;
      cols(cursor) = i;
      cursor++;
    }
  }
  assert(cursor == AbstractShot::getNumberNonZeroJacobian());
}

//==============================================================================
/// This writes the Jacobian to a sparse vector
void AbstractShot::getSparseJacobian(
    std::shared_ptr<simulation::World> world,
    Eigen::Ref<Eigen::VectorXd> sparse)
{
  assert(sparse.size() == AbstractShot::getNumberNonZeroJacobian());

  sparse.setZero();

  int cursor = 0;
  int n = getFlatProblemDim();
  for (int i = 0; i < mConstraints.size(); i++)
  {
    mConstraints[i].getLossAndGradient(
        getRolloutCache(world),
        /* OUT */ getGradientWrtRolloutCache(world));
    backpropGradientWrt(
        world,
        getGradientWrtRolloutCache(world),
        /* OUT */ sparse.segment(cursor, n));
    cursor += n;
  }

  assert(cursor == sparse.size());
}

//==============================================================================
/// This computes the gradient in the flat problem space, automatically
/// computing the gradients of the loss function as part of the call
void AbstractShot::backpropGradient(
    std::shared_ptr<simulation::World> world,
    /* OUT */ Eigen::Ref<Eigen::VectorXd> grad)
{
  mLoss.getLossAndGradient(
      getRolloutCache(world),
      /* OUT */ getGradientWrtRolloutCache(world));
  backpropGradientWrt(
      world,
      getGradientWrtRolloutCache(world),
      /* OUT */ grad);
}

//==============================================================================
/// Get the loss for the rollout
double AbstractShot::getLoss(std::shared_ptr<simulation::World> world)
{
  return mLoss.getLoss(getRolloutCache(world));
}

//==============================================================================
const TrajectoryRollout* AbstractShot::getRolloutCache(
    std::shared_ptr<simulation::World> world, bool useKnots)
{
  if (mRolloutCacheDirty)
  {
    mRolloutCache = std::make_shared<TrajectoryRolloutReal>(this);
    getStates(
        world,
        /* OUT */ mRolloutCache.get());
    mGradWrtRolloutCache = std::make_shared<TrajectoryRolloutReal>(this);
    mRolloutCacheDirty = false;
  }
  return mRolloutCache.get();
}

//==============================================================================
TrajectoryRollout* AbstractShot::getGradientWrtRolloutCache(
    std::shared_ptr<simulation::World> world, bool useKnots)
{
  if (mRolloutCacheDirty)
  {
    mRolloutCache = std::make_shared<TrajectoryRolloutReal>(this);
    getStates(
        world,
        /* OUT */ mRolloutCache.get());
    mGradWrtRolloutCache = std::make_shared<TrajectoryRolloutReal>(this);
    mRolloutCacheDirty = false;
  }
  return mGradWrtRolloutCache.get();
}

//==============================================================================
/// This computes finite difference Jacobians analagous to
/// backpropGradient()
void AbstractShot::finiteDifferenceGradient(
    std::shared_ptr<simulation::World> world,
    /* OUT */ Eigen::Ref<Eigen::VectorXd> grad)
{
  double originalLoss = mLoss.getLoss(getRolloutCache(world));

  int dims = getFlatProblemDim();
  Eigen::VectorXd flat = Eigen::VectorXd::Zero(dims);
  flatten(flat);

  assert(grad.size() == dims);

  const double EPS = 1e-6;

  for (int i = 0; i < dims; i++)
  {
    flat(i) += EPS;
    unflatten(flat);
    double posLoss = mLoss.getLoss(getRolloutCache(world));
    flat(i) -= EPS;

    flat(i) -= EPS;
    unflatten(flat);
    double negLoss = mLoss.getLoss(getRolloutCache(world));
    flat(i) += EPS;

    grad(i) = (posLoss - negLoss) / (2 * EPS);
  }
}

//==============================================================================
int AbstractShot::getNumSteps()
{
  return mSteps;
}

//==============================================================================
/// This computes finite difference Jacobians analagous to backpropJacobians()
void AbstractShot::finiteDifferenceJacobian(
    std::shared_ptr<simulation::World> world, Eigen::Ref<Eigen::MatrixXd> jac)
{
  int dim = getFlatProblemDim();
  int numConstraints = getConstraintDim();
  assert(jac.cols() == dim);
  assert(jac.rows() == numConstraints);

  Eigen::VectorXd originalConstraints = Eigen::VectorXd::Zero(numConstraints);
  computeConstraints(world, originalConstraints);
  Eigen::VectorXd flat = Eigen::VectorXd::Zero(dim);
  flatten(flat);

  const double EPS = 1e-7;

  Eigen::VectorXd positiveConstraints = Eigen::VectorXd::Zero(numConstraints);
  Eigen::VectorXd negativeConstraints = Eigen::VectorXd::Zero(numConstraints);
  for (int i = 0; i < dim; i++)
  {
    flat(i) += EPS;
    unflatten(flat);
    computeConstraints(world, positiveConstraints);
    flat(i) -= EPS;

    flat(i) -= EPS;
    unflatten(flat);
    computeConstraints(world, negativeConstraints);
    flat(i) += EPS;

    jac.col(i) = (positiveConstraints - negativeConstraints) / (2 * EPS);
  }
}

} // namespace trajectory
} // namespace dart