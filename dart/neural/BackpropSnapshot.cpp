#include "dart/neural/BackpropSnapshot.hpp"

#include <iostream>

#include "dart/constraint/ConstraintSolver.hpp"
#include "dart/dynamics/Skeleton.hpp"
#include "dart/neural/ConstrainedGroupGradientMatrices.hpp"
#include "dart/neural/DifferentiableContactConstraint.hpp"
#include "dart/neural/RestorableSnapshot.hpp"
#include "dart/simulation/World.hpp"

// This replaces all analytical Jacobian calculations with finite differencing.
//
// #define USE_FD_OVERRIDE

// This will enable runtime checks where every analytical Jacobian is compared
// to the finite difference version, and we error if they're too close far
// apart:
//
// #define SLOW_FD_CHECK_EVERYTHING

using namespace dart;
using namespace math;
using namespace dynamics;
using namespace simulation;

namespace dart {
namespace neural {

//==============================================================================
BackpropSnapshot::BackpropSnapshot(
    WorldPtr world,
    Eigen::VectorXd preStepPosition,
    Eigen::VectorXd preStepVelocity,
    Eigen::VectorXd preStepTorques,
    Eigen::VectorXd preConstraintVelocities)
{
  mTimeStep = world->getTimeStep();
  mPreStepPosition = preStepPosition;
  mPreStepVelocity = preStepVelocity;
  mPreStepTorques = preStepTorques;
  mPreConstraintVelocities = preConstraintVelocities;
  mPostStepPosition = world->getPositions();
  mPostStepVelocity = world->getVelocities();
  mPostStepTorques = world->getForces();
  mNumDOFs = 0;
  mNumConstraintDim = 0;
  mNumClamping = 0;
  mNumUpperBound = 0;
  mNumBouncing = 0;

  // Reset the world to the initial state before finalizing all the gradient
  // matrices

  RestorableSnapshot snapshot(world);
  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  // Collect all the constraint groups attached to each skeleton

  for (std::size_t i = 0; i < world->getNumSkeletons(); i++)
  {
    SkeletonPtr skel = world->getSkeleton(i);
    mSkeletonOffset[skel->getName()] = mNumDOFs;
    mSkeletonDofs[skel->getName()] = skel->getNumDofs();
    mNumDOFs += skel->getNumDofs();

    std::shared_ptr<ConstrainedGroupGradientMatrices> gradientMatrix
        = skel->getGradientConstraintMatrices();
    if (gradientMatrix
        && std::find(
               mGradientMatrices.begin(),
               mGradientMatrices.end(),
               gradientMatrix)
               == mGradientMatrices.end())
    {
      // Finalize the construction of the matrices
      gradientMatrix->constructMatrices(world);
      mGradientMatrices.push_back(gradientMatrix);
      mNumConstraintDim += gradientMatrix->getNumConstraintDim();
      mNumClamping += gradientMatrix->getClampingConstraintMatrix().cols();
      mNumUpperBound += gradientMatrix->getUpperBoundConstraintMatrix().cols();
      mNumBouncing += gradientMatrix->getBouncingConstraintMatrix().cols();
    }
  }

  snapshot.restore();

  mCachedPosPosDirty = true;
  mCachedPosVelDirty = true;
  mCachedVelPosDirty = true;
  mCachedVelVelDirty = true;
  mCachedForcePosDirty = true;
  mCachedForceVelDirty = true;
}

//==============================================================================
void BackpropSnapshot::backprop(
    WorldPtr world,
    LossGradient& thisTimestepLoss,
    const LossGradient& nextTimestepLoss)
{
  LossGradient groupThisTimestepLoss;
  LossGradient groupNextTimestepLoss;

  // Set the state of the world back to what it was during the forward pass, so
  // that implicit mass matrix computations work correctly.

  Eigen::VectorXd oldPositions = world->getPositions();
  Eigen::VectorXd oldVelocities = world->getVelocities();
  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);

  // Create the vectors for this timestep

  thisTimestepLoss.lossWrtPosition = Eigen::VectorXd(mNumDOFs);
  thisTimestepLoss.lossWrtVelocity = Eigen::VectorXd(mNumDOFs);
  thisTimestepLoss.lossWrtTorque = Eigen::VectorXd(mNumDOFs);

  // TODO: <remove me>

  const Eigen::MatrixXd& posPos = getPosPosJacobian(world);
  const Eigen::MatrixXd& posVel = getPosVelJacobian(world);
  const Eigen::MatrixXd& velPos = getVelPosJacobian(world);
  const Eigen::MatrixXd& velVel = getVelVelJacobian(world);
  const Eigen::MatrixXd& forceVel = getForceVelJacobian(world);

  thisTimestepLoss.lossWrtPosition
      = posPos.transpose() * nextTimestepLoss.lossWrtPosition
        + posVel.transpose() * nextTimestepLoss.lossWrtVelocity;
  thisTimestepLoss.lossWrtVelocity
      = velPos.transpose() * nextTimestepLoss.lossWrtPosition
        + velVel.transpose() * nextTimestepLoss.lossWrtVelocity;
  thisTimestepLoss.lossWrtTorque
      = forceVel.transpose() * nextTimestepLoss.lossWrtVelocity;

  return;

  // TODO: </remove me>

  // Actually run the backprop

  std::unordered_map<std::string, bool> skeletonsVisited;

  for (std::shared_ptr<ConstrainedGroupGradientMatrices> group :
       mGradientMatrices)
  {
    std::size_t groupDofs = group->getNumDOFs();

    // Instantiate the vectors with plenty of DOFs

    groupNextTimestepLoss.lossWrtPosition = Eigen::VectorXd(groupDofs);
    groupNextTimestepLoss.lossWrtVelocity = Eigen::VectorXd(groupDofs);
    groupThisTimestepLoss.lossWrtPosition = Eigen::VectorXd(groupDofs);
    groupThisTimestepLoss.lossWrtVelocity = Eigen::VectorXd(groupDofs);
    groupThisTimestepLoss.lossWrtTorque = Eigen::VectorXd(groupDofs);

    // Set up next timestep loss as a map of the real values

    std::size_t cursor = 0;
    for (std::size_t j = 0; j < group->getSkeletons().size(); j++)
    {
      SkeletonPtr skel = world->getSkeleton(group->getSkeletons()[j]);
      std::size_t dofCursorWorld = mSkeletonOffset[skel->getName()];
      std::size_t dofs = skel->getNumDofs();

      // Keep track of which skeletons have been covered by constraint groups
      bool skelAlreadyVisited
          = (skeletonsVisited.find(skel->getName()) != skeletonsVisited.end());
      assert(!skelAlreadyVisited);
      skeletonsVisited[skel->getName()] = true;

      groupNextTimestepLoss.lossWrtPosition.segment(cursor, dofs)
          = nextTimestepLoss.lossWrtPosition.segment(dofCursorWorld, dofs);
      groupNextTimestepLoss.lossWrtVelocity.segment(cursor, dofs)
          = nextTimestepLoss.lossWrtVelocity.segment(dofCursorWorld, dofs);

      cursor += dofs;
    }

    // Now actually run the backprop

    group->backprop(world, groupThisTimestepLoss, groupNextTimestepLoss);

    // Read the values back out of the group backprop

    cursor = 0;
    for (std::size_t j = 0; j < group->getSkeletons().size(); j++)
    {
      SkeletonPtr skel = world->getSkeleton(group->getSkeletons()[j]);
      std::size_t dofCursorWorld = mSkeletonOffset[skel->getName()];
      std::size_t dofs = skel->getNumDofs();

      thisTimestepLoss.lossWrtPosition.segment(dofCursorWorld, dofs)
          = groupThisTimestepLoss.lossWrtPosition.segment(cursor, dofs);
      thisTimestepLoss.lossWrtVelocity.segment(dofCursorWorld, dofs)
          = groupThisTimestepLoss.lossWrtVelocity.segment(cursor, dofs);
      thisTimestepLoss.lossWrtTorque.segment(dofCursorWorld, dofs)
          = groupThisTimestepLoss.lossWrtTorque.segment(cursor, dofs);

      cursor += dofs;
    }
  }

  // We need to go through and manually cover any skeletons that aren't covered
  // by any constraint group (because they have no active constraints). Because
  // these skeletons aren't part of a constrained group, their Jacobians are
  // quite simple.

  for (std::size_t i = 0; i < world->getNumSkeletons(); i++)
  {
    SkeletonPtr skel = world->getSkeleton(i);
    bool skelAlreadyVisited
        = (skeletonsVisited.find(skel->getName()) != skeletonsVisited.end());
    if (!skelAlreadyVisited && skel->isMobile())
    {
      std::size_t dofCursorWorld = mSkeletonOffset[skel->getName()];
      std::size_t dofs = skel->getNumDofs();
      // f_t
      // force-vel = dT * Minv
      thisTimestepLoss.lossWrtTorque.segment(dofCursorWorld, dofs)
          // f_t --> v_t+1
          = mTimeStep
            * skel->multiplyByImplicitInvMassMatrix(
                nextTimestepLoss.lossWrtVelocity.segment(dofCursorWorld, dofs));

      // skel->getJacobionOfMinv();
      // getUnconstrainedVelJacobianWrt
      // p_t
      // pos-pos = I
      // pos-vel = dT * Minv * d/dpos C(pos,vel) + dT * d/dpos Minv * C(pos,
      // vel) pos-vel^T = dT * d/dpos C(pos,vel)^T * Minv
      thisTimestepLoss.lossWrtPosition.segment(dofCursorWorld, dofs)
          // p_t --> p_t+1
          = nextTimestepLoss.lossWrtPosition.segment(dofCursorWorld, dofs)
            // p_t --> v_t+1
            + (skel->getUnconstrainedVelJacobianWrt(
                       world->getTimeStep(), WithRespectTo::POSITION)
                   .transpose()
               * nextTimestepLoss.lossWrtVelocity.segment(
                   dofCursorWorld, dofs));

      // v_t
      // vel-vel = I - dT * Minv * d/dvel C(pos,vel)
      // vel-pos = dT * I
      thisTimestepLoss.lossWrtVelocity.segment(dofCursorWorld, dofs)
          // v_t --> v_t+1
          = nextTimestepLoss.lossWrtVelocity.segment(dofCursorWorld, dofs)
            - skel->getVelCJacobian().transpose()
                  * thisTimestepLoss.lossWrtTorque.segment(dofCursorWorld, dofs)
            // v_t --> p_t
            + mTimeStep
                  * thisTimestepLoss.lossWrtPosition.segment(
                      dofCursorWorld, dofs);
    }
  }

  // Restore the old position and velocity values before we ran backprop
  world->setPositions(oldPositions);
  world->setVelocities(oldVelocities);
}

//==============================================================================
const Eigen::MatrixXd& BackpropSnapshot::getForceVelJacobian(WorldPtr world)
{
  if (mCachedForceVelDirty)
  {
#ifdef USE_FD_OVERRIDE
    mCachedForceVel = finiteDifferenceForceVelJacobian(world);
#else
    Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
    Eigen::MatrixXd Minv = getInvMassMatrix(world);

    // If there are no clamping constraints, then force-vel is just the
    // mTimeStep
    // * Minv
    if (A_c.size() == 0)
    {
      mCachedForceVel = mTimeStep * Minv;
    }
    else
    {
      Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
      Eigen::MatrixXd E = getUpperBoundMappingMatrix();
      Eigen::MatrixXd P_c = getProjectionIntoClampsMatrix(world);

      if (A_ub.size() > 0 && E.size() > 0)
      {
        mCachedForceVel = mTimeStep * Minv
                          * (Eigen::MatrixXd::Identity(mNumDOFs, mNumDOFs)
                             - mTimeStep * (A_c + A_ub * E) * P_c * Minv);
      }
      else
      {
        mCachedForceVel = mTimeStep * Minv
                          * (Eigen::MatrixXd::Identity(mNumDOFs, mNumDOFs)
                             - mTimeStep * A_c * P_c * Minv);
      }
    }
#endif

#ifdef SLOW_FD_CHECK_EVERYTHING
    Eigen::MatrixXd bruteForce = finiteDifferenceForceVelJacobian(world);
    equalsOrCrash(world, mCachedForceVel, bruteForce, "force-vel");
#endif

    // mCachedForceVel = getVelJacobianWrt(world, WithRespectTo::FORCE);
    mCachedForceVelDirty = false;
  }
  return mCachedForceVel;
}

//==============================================================================
const Eigen::MatrixXd& BackpropSnapshot::getVelVelJacobian(WorldPtr world)
{
  if (mCachedVelVelDirty)
  {
#ifdef USE_FD_OVERRIDE
    mCachedVelVel = finiteDifferenceVelVelJacobian(world);
#else
    Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);

    // If there are no clamping constraints, then vel-vel is just the identity
    if (A_c.size() == 0)
    {
      mCachedVelVel = Eigen::MatrixXd::Identity(mNumDOFs, mNumDOFs)
                      - getForceVelJacobian(world) * getVelCJacobian(world);
    }
    else
    {
      // mCachedVelVel = getVelJacobianWrt(world, WithRespectTo::VELOCITY);

      Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
      Eigen::MatrixXd E = getUpperBoundMappingMatrix();
      Eigen::MatrixXd P_c = getProjectionIntoClampsMatrix(world);
      Eigen::MatrixXd Minv = getInvMassMatrix(world);
      Eigen::MatrixXd parts1 = A_c + A_ub * E;
      Eigen::MatrixXd parts2 = mTimeStep * Minv * parts1 * P_c;

      mCachedVelVel = (Eigen::MatrixXd::Identity(mNumDOFs, mNumDOFs) - parts2)
                      - getForceVelJacobian(world) * getVelCJacobian(world);

      /*
      std::cout << "A_c: " << std::endl << A_c << std::endl;
      std::cout << "A_ub: " << std::endl << A_ub << std::endl;
      std::cout << "E: " << std::endl << E << std::endl;
      std::cout << "P_c: " << std::endl << P_c << std::endl;
      std::cout << "Minv: " << std::endl << Minv << std::endl;
      std::cout << "mTimestep: " << mTimeStep << std::endl;
      std::cout << "A_c + A_ub * E: " << std::endl << parts1 << std::endl;
      */
      /*
       std::cout << "Vel-vel construction pieces: " << std::endl;
       std::cout << "1: I " << std::endl
                 << Eigen::MatrixXd::Identity(mNumDOFs, mNumDOFs) << std::endl;
       std::cout << "2: - mTimestep * Minv * (A_c + A_ub * E) * P_c" <<
       std::endl
                 << -parts2 << std::endl;
       std::cout << "2.5: velC" << std::endl << getVelCJacobian(world) <<
       std::endl; std::cout << "3: - forceVel * velC" << std::endl
                 << -getForceVelJacobian(world) * getVelCJacobian(world)
                 << std::endl;
       */
    }
#endif

#ifdef SLOW_FD_CHECK_EVERYTHING
    Eigen::MatrixXd bruteForce = finiteDifferenceVelVelJacobian(world);
    equalsOrCrash(world, mCachedVelVel, bruteForce, "vel-vel");
#endif

    mCachedVelVelDirty = false;
  }
  return mCachedVelVel;
}

//==============================================================================
const Eigen::MatrixXd& BackpropSnapshot::getPosVelJacobian(WorldPtr world)
{
  if (mCachedPosVelDirty)
  {
#ifdef USE_FD_OVERRIDE
    mCachedPosVel = finiteDifferencePosVelJacobian(world);
#else
    mCachedPosVel = getVelJacobianWrt(world, WithRespectTo::POSITION);
#endif

#ifdef SLOW_FD_CHECK_EVERYTHING
    Eigen::MatrixXd bruteForce = finiteDifferencePosVelJacobian(world);
    equalsOrCrash(world, mCachedPosVel, bruteForce, "pos-vel");
#endif

    mCachedPosVelDirty = false;
  }
  return mCachedPosVel;
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getAnalyticalNextV(
    simulation::WorldPtr world, bool morePreciseButSlower)
{
  Eigen::MatrixXd A_c
      = morePreciseButSlower
            ? getClampingConstraintMatrixAt(world, world->getPositions())
            : estimateClampingConstraintMatrixAt(world, world->getPositions());
  Eigen::MatrixXd A_ub
      = morePreciseButSlower
            ? getUpperBoundConstraintMatrixAt(world, world->getPositions())
            : estimateUpperBoundConstraintMatrixAt(
                world, world->getPositions());
  Eigen::MatrixXd E = getUpperBoundMappingMatrix();
  Eigen::MatrixXd A_c_ub_E = A_c + A_ub * E;
  Eigen::MatrixXd P_c = getProjectionIntoClampsMatrix(world, true);

  Eigen::MatrixXd Minv = world->getInvMassMatrix();
  Eigen::VectorXd tau = world->getForces();
  Eigen::VectorXd C = world->getCoriolisAndGravityAndExternalForces();
  double dt = world->getTimeStep();
  Eigen::VectorXd f_c = estimateClampingConstraintImpulses(world, A_c);

  Eigen::VectorXd preSolveV = mPreStepVelocity + dt * Minv * (tau - C);
  Eigen::VectorXd f_cDeltaV = Minv * A_c_ub_E * f_c;
  Eigen::VectorXd postSolveV = preSolveV + f_cDeltaV;
  return postSolveV;

  /*
  Eigen::VectorXd innerV = world->getVelocities() + dt * Minv * (tau - C);

  return world->getVelocities()
         + dt * Minv * (tau - C - A_c_ub_E * P_c * innerV);
  */
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getScratchAnalytical(
    simulation::WorldPtr world)
{
  RestorableSnapshot snapshot(world);
  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  if (A_c.size() == 0)
    return Eigen::MatrixXd::Zero(0, world->getNumDofs());
  Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
  Eigen::MatrixXd E = getUpperBoundMappingMatrix();

  Eigen::MatrixXd Minv = getInvMassMatrix(world, false);
  Eigen::MatrixXd A_c_ub_E = A_c + (A_ub * E);
  Eigen::MatrixXd constraintForceToImpliedTorques = Minv * A_c_ub_E;
  Eigen::MatrixXd forceToVel
      = A_c.eval().transpose() * constraintForceToImpliedTorques;
  Eigen::MatrixXd Q = A_c.eval().transpose() * constraintForceToImpliedTorques;
  auto XFactor = Q.completeOrthogonalDecomposition();
  Eigen::MatrixXd bounce = getBounceDiagonals().asDiagonal();

  Eigen::VectorXd v = Eigen::VectorXd::Ones(mNumDOFs);

  // d/d Q^{-1} v = - Q^{-1} (d/d Q) Q^{-1} v
  Eigen::MatrixXd rightHandSide = bounce * A_c.transpose();
  Eigen::MatrixXd dRhs
      = bounce * getJacobianOfClampingConstraintsTranspose(world, v);

  Eigen::MatrixXd Qinv = XFactor.pseudoInverse();
  Eigen::VectorXd Qinv_v = XFactor.solve(rightHandSide * v);
  Eigen::MatrixXd dQ
      = getJacobianOfClampingConstraintsTranspose(
            world, Minv * A_c_ub_E * Qinv_v)
        + A_c.transpose()
              * (getJacobianOfMinv(world, A_c_ub_E * Qinv_v, POSITION)
                 + Minv * getJacobianOfClampingConstraints(world, Qinv_v));

  return (1 / world->getTimeStep()) * (XFactor.solve(dRhs) - XFactor.solve(dQ));

  // Approximate the pseudo-inverse as just a plain inverse for the purposes of
  // derivation

  /*
  Eigen::VectorXd tau = A_c_ub_E * XFactor.solve(bounce * A_c.transpose() * v);

  Eigen::MatrixXd MinvJac
      = getJacobianOfMinv(world, tau, WithRespectTo::POSITION);
      */
  /*
  return -(1.0 / world->getTimeStep())
         * XFactor.solve(A_c.transpose() * MinvJac);
  */

  /*
  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
  Eigen::MatrixXd E = getUpperBoundMappingMatrix();
  Eigen::MatrixXd A_c_ub_E = A_c + A_ub * E;

  Eigen::VectorXd tau = world->getForces();
  Eigen::VectorXd C = world->getCoriolisAndGravityAndExternalForces();
  Eigen::VectorXd f_c
      = getClampingConstraintImpulses(); // estimateClampingConstraintImpulses(
                                         // world, A_c); //
                                         // getClampingConstraintImpulses();
  // Eigen::VectorXd f_cReal = getClampingConstraintImpulses();
  // std::cout << "f_c estimate: " << std::endl << f_c << std::endl;
  // std::cout << "f_c real: " << std::endl << f_cReal << std::endl;
  double dt = world->getTimeStep();
  Eigen::MatrixXd dM = getJacobianOfMinv(
      world, dt * (tau - C) + A_c_ub_E * f_c, WithRespectTo::POSITION);

  Eigen::MatrixXd Minv = world->getInvMassMatrix();

  Eigen::MatrixXd dA_c = getJacobianOfClampingConstraints(world, f_c);
  Eigen::MatrixXd dA_ubE = getJacobianOfUpperBoundConstraints(world, E * f_c);

  Eigen::VectorXd c = Eigen::VectorXd::Ones(mNumDOFs);
  Eigen::VectorXd Minv_c = Minv.completeOrthogonalDecomposition().solve(c);
  Eigen::MatrixXd dMinv
      = getJacobianOfMinv(world, Minv_c, WithRespectTo::POSITION);

  // std::cout << "dQ_b: " << std::endl << dQ_b << std::endl;
  // return dF_c;

  // return dQ_b;

  // return -Minv.completeOrthogonalDecomposition().solve(dMinv);

  Eigen::MatrixXd partQ = Minv * Minv; // * A_c;
  Eigen::VectorXd partQ_c = partQ.completeOrthogonalDecomposition().solve(c);
  Eigen::MatrixXd dMinv1
      = Minv * getJacobianOfMinv(world, partQ_c, WithRespectTo::POSITION);
  Eigen::MatrixXd dMinv2
      = getJacobianOfMinv(world, Minv * partQ_c, WithRespectTo::POSITION);
  // dA_c = getJacobianOfClampingConstraints(world, partQ_c);

  // return -partQ.completeOrthogonalDecomposition().solve(dMinv1 + dMinv2);

  // return dA_c_T + A_c.transpose() * dMinv + A_c.transpose() * Minv * dA_c;

  // return dQ_b;

  // return dF_c;

  Eigen::VectorXd f = world->getForces() - C;
  Eigen::MatrixXd dMinv_f
      = getJacobianOfMinv(world, f, WithRespectTo::POSITION);
  Eigen::VectorXd v_f = world->getVelocities()
                        // + getVelocityDueToIllegalImpulses()
                        + (world->getTimeStep() * Minv * f);
  Eigen::MatrixXd dA_c_f
      = getJacobianOfClampingConstraintsTranspose(world, v_f);

  Eigen::MatrixXd Q
      = getClampingAMatrix(); // Eigen::MatrixXd(A_c.cols(), A_c.cols());
  // computeLCPConstraintMatrixClampingSubset(world, Q, A_c);
  Eigen::VectorXd b
      = getClampingConstraintRelativeVels(); //
  Eigen::VectorXd::Ones(A_c.cols());
  // computeLCPOffsetClampingSubset(world, b, A_c);
  Eigen::MatrixXd dQ_b = getJacobianOfLCPConstraintMatrixClampingSubset(
      world, b, WithRespectTo::POSITION);

  Eigen::MatrixXd dB
      = getJacobianOfLCPOffsetClampingSubset(world, WithRespectTo::POSITION);

  Eigen::MatrixXd dF_c
      = getJacobianOfConstraintForce(world, WithRespectTo::POSITION);

  Eigen::MatrixXd dC = getJacobianOfC(
      world, WithRespectTo::POSITION); // dC = getVelCJacobian(world);

  // return A_c * dF_c;

  dA_c = getJacobianOfClampingConstraints(world, f_c);
  dA_ubE = getJacobianOfUpperBoundConstraints(world, E * f_c);
  snapshot.restore();

  Eigen::MatrixXd dMinvC = getJacobianOfMinvC(world, WithRespectTo::POSITION);

  return dMinvC;

  return dMinv_f - Minv * dC;

  return dB;

  return dF_c;

  return dM + Minv * (A_c_ub_E * dF_c + dA_c + dA_ubE - dt * dC);
  */

  /*
  return Minv
         * (dt
                * Eigen::MatrixXd::Identity(
                    world->getNumDofs(), world->getNumDofs())
            + A_c * dF_c);
            */

  /*
  return Eigen::MatrixXd::Identity(world->getNumDofs(), world->getNumDofs())
         + dt * Minv * (dC + A_c * dF_c);
  */

  // dC + A_c * dF_c; // dQ_b + Q.completeOrthogonalDecomposition().solve(dB);
  // -(dA_c_f + A_c.transpose() * dt * (dMinv_f - Minv * dC));

  // return dA_c2 + A_c * dF_c;

  // return dF_c;

  // return dM + Minv * (A_c_ub_E * dF_c + dA_c + dA_ubE - dt * dC);
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::scratch(simulation::WorldPtr world)
{
  Eigen::MatrixXd newP_c;

  Eigen::MatrixXd A_c
      = getClampingConstraintMatrixAt(world, world->getPositions());
  if (A_c.size() == 0)
    return Eigen::MatrixXd::Zero(0, world->getNumDofs());

  Eigen::MatrixXd E = getUpperBoundMappingMatrix();

  Eigen::MatrixXd constraintForceToImpliedTorques;
  bool forFiniteDifferencing = true;
  if (forFiniteDifferencing || true)
  {
    Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
    Eigen::MatrixXd Minv = getInvMassMatrix(world, forFiniteDifferencing);
    constraintForceToImpliedTorques = Minv * (A_c + (A_ub * E));
  }
  else
  {
    Eigen::MatrixXd V_c = getMassedClampingConstraintMatrix(world);
    Eigen::MatrixXd V_ub = getMassedUpperBoundConstraintMatrix(world);
    constraintForceToImpliedTorques = V_c + (V_ub * E);
  }

  Eigen::MatrixXd forceToVel
      = A_c.eval().transpose() * constraintForceToImpliedTorques;
  Eigen::MatrixXd bounce = getBounceDiagonals().asDiagonal();
  Eigen::MatrixXd rightHandSide = bounce * A_c.transpose();
  /*
  std::cout << "forceToVel: " << std::endl << forceToVel << std::endl;
  std::cout << "forceToVel^-1: " << std::endl << velToForce << std::endl;
  std::cout << "mTimeStep: " << mTimeStep << std::endl;
  */
  newP_c = (1.0 / mTimeStep)
           * forceToVel.completeOrthogonalDecomposition().solve(rightHandSide);

  Eigen::VectorXd v = Eigen::VectorXd::Ones(mNumDOFs);
  Eigen::MatrixXd Qinv
      = forceToVel.completeOrthogonalDecomposition().pseudoInverse();

  // return Qinv * v;
  // return Qinv * rightHandSide * v;
  return newP_c * v;

  /*
  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
  Eigen::MatrixXd E = getUpperBoundMappingMatrix();
  Eigen::MatrixXd A_c_ub_E = A_c + A_ub * E;

  Eigen::MatrixXd Minv = world->getInvMassMatrix();
  Eigen::VectorXd f_c = estimateClampingConstraintImpulses(world);
  Eigen::VectorXd tau = world->getForces();
  Eigen::VectorXd C = world->getCoriolisAndGravityAndExternalForces();
  double dt = world->getTimeStep();
  Eigen::VectorXd nextV
      = world->getVelocities() + Minv * (dt * (tau - C) + A_c_ub_E * f_c);

  return nextV;
  */

  /*
  // Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  Eigen::MatrixXd A_c = estimateClampingConstraintMatrixAt(
      world, world->getPositions()); // getClampingConstraintMatrix(world);
  // A_c = estimateClampingConstraintMatrixAt(world, world->getPositions());
  Eigen::MatrixXd A_ub = estimateUpperBoundConstraintMatrixAt(
      world, world->getPositions()); // getClampingConstraintMatrix(world);
  Eigen::MatrixXd E = getUpperBoundMappingMatrix();
  Eigen::MatrixXd A_c_ub_E = A_c + A_ub * E;

  Eigen::MatrixXd Minv = world->getInvMassMatrix();
  Eigen::VectorXd tau = world->getForces();
  Eigen::VectorXd C = world->getCoriolisAndGravityAndExternalForces();
  double dt = world->getTimeStep();
  Eigen::VectorXd f_c = estimateClampingConstraintImpulses(world, A_c);

  Eigen::MatrixXd Q = Eigen::MatrixXd(A_c.cols(), A_c.cols());
  computeLCPConstraintMatrixClampingSubset(world, Q, A_c);

  Eigen::MatrixXd partQ = A_c.transpose() * Minv;

  // return partQ * b;

  Eigen::VectorXd c = Eigen::VectorXd::Ones(mNumDOFs);
  // return partQ.completeOrthogonalDecomposition().solve(c);

  Eigen::MatrixXd realQ = getClampingAMatrix();
  Eigen::MatrixXd realA_c = getClampingConstraintMatrix(world);
  Eigen::VectorXd b = Eigen::VectorXd::Ones(realA_c.cols());
  computeLCPOffsetClampingSubset(world, b, realA_c);

  Eigen::VectorXd realF_c = realQ.completeOrthogonalDecomposition().solve(b);
  // Q.completeOrthogonalDecomposition().solve(b);

  // return realA_c * realF_c;

  // return world->getVelocities() + Minv * (dt * tau + dt * C + (A_c * f_c));

  // return b;

  // return A_c * f_c;

  return Minv * (tau - C);

  BackpropSnapshotPtr ptr = neural::forwardPass(world, true);

  return ptr->getClampingConstraintRelativeVels();

  Eigen::VectorXd preSolveV = mPreStepVelocity + dt * Minv * (tau - C);
  Eigen::VectorXd f_cDeltaV = Minv * A_c_ub_E * f_c;
  Eigen::VectorXd postSolveV = preSolveV + f_cDeltaV;
  return postSolveV;
  */

  /*
  return world->getVelocities()
         + dt * Minv * (tau - C - A_c_ub_E * P_c * innerV);
         */
}

Eigen::MatrixXd BackpropSnapshot::getScratchFiniteDifference(
    simulation::WorldPtr world)
{
  RestorableSnapshot snapshot(world);

  bool oldGradientEnabled = world->getConstraintSolver()->getGradientEnabled();
  bool oldPenetrationCorrectionEnabled
      = world->getConstraintSolver()->getPenetrationCorrectionEnabled();
  world->getConstraintSolver()->setGradientEnabled(false);
  world->getConstraintSolver()->setPenetrationCorrectionEnabled(false);

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  Eigen::VectorXd original = scratch(world);

  Eigen::MatrixXd J = Eigen::MatrixXd(original.size(), world->getNumDofs());

  double EPSILON = 1e-7;
  for (std::size_t i = 0; i < world->getNumDofs(); i++)
  {
    Eigen::VectorXd tweakedPos = mPreStepPosition;
    tweakedPos(i) += EPSILON;
    world->setPositions(tweakedPos);
    Eigen::VectorXd perturbedPos = scratch(world);

    tweakedPos = mPreStepPosition;
    tweakedPos(i) -= EPSILON;
    world->setPositions(tweakedPos);
    Eigen::VectorXd perturbedNeg = scratch(world);

    Eigen::VectorXd change = (perturbedPos - perturbedNeg) / (2 * EPSILON);
    J.col(i).noalias() = change;
  }

  snapshot.restore();
  world->getConstraintSolver()->setGradientEnabled(oldGradientEnabled);
  world->getConstraintSolver()->setPenetrationCorrectionEnabled(
      oldPenetrationCorrectionEnabled);

  return J;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getVelJacobianWrt(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  RestorableSnapshot snapshot(world);
  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
  Eigen::MatrixXd E = getUpperBoundMappingMatrix();
  Eigen::MatrixXd A_c_ub_E = A_c + A_ub * E;

  Eigen::VectorXd tau = world->getForces();
  Eigen::VectorXd C = world->getCoriolisAndGravityAndExternalForces();
  Eigen::VectorXd f_c = getClampingConstraintImpulses();
  double dt = world->getTimeStep();

  Eigen::MatrixXd dM
      = getJacobianOfMinv(world, dt * (tau - C) + A_c_ub_E * f_c, wrt);

  Eigen::MatrixXd Minv = world->getInvMassMatrix();
  Eigen::MatrixXd dC = getJacobianOfC(world, wrt);

  Eigen::MatrixXd dF_c = getJacobianOfConstraintForce(world, wrt);

  if (wrt == WithRespectTo::VELOCITY)
  {
    snapshot.restore();
    return Eigen::MatrixXd::Identity(world->getNumDofs(), world->getNumDofs())
           + Minv * (dt * dC + A_c * dF_c);
  }
  else if (wrt == WithRespectTo::FORCE)
  {
    snapshot.restore();
    return Minv
           * (dt
                  * Eigen::MatrixXd::Identity(
                      world->getNumDofs(), world->getNumDofs())
              + A_c * dF_c);
  }
  else if (wrt == WithRespectTo::POSITION)
  {
    Eigen::MatrixXd dA_c = getJacobianOfClampingConstraints(world, f_c);
    Eigen::MatrixXd dA_ubE = getJacobianOfUpperBoundConstraints(world, E * f_c);
    snapshot.restore();
    return dM + Minv * (A_c_ub_E * dF_c + dA_c + dA_ubE - dt * dC);
  }
  else
  {
    snapshot.restore();
    return dM + Minv * (A_c_ub_E * dF_c - dt * dC);
  }

  // std::cout << "dA_c: " << std::endl << dA_c << std::endl;

  // return dM + Minv * (dA_c + A_c_ub_E * dF_c - dt * dC);

  // Old version
  /*
{
  RestorableSnapshot snapshot(world);
  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  Eigen::VectorXd tau = world->getForces();
  Eigen::VectorXd C = world->getCoriolisAndGravityAndExternalForces();
  Eigen::MatrixXd dM = getJacobianOfMinv(world, tau - C, wrt);
  Eigen::MatrixXd Minv = world->getInvMassMatrix();
  Eigen::MatrixXd dC = getJacobianOfC(world, wrt);
  double dt = world->getTimeStep();
  Eigen::VectorXd innerV = world->getVelocities() + dt * Minv * (tau - C);

  Eigen::MatrixXd dP_c
      = getJacobianOfProjectionIntoClampsMatrix(world, innerV, wrt);
  Eigen::MatrixXd P_c = getProjectionIntoClampsMatrix(world);
  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
  Eigen::MatrixXd E = getUpperBoundMappingMatrix();
  Eigen::MatrixXd A_c_ub_E = A_c + A_ub * E;

  Eigen::VectorXd outerTau = tau - C - A_c_ub_E * P_c * innerV;
  Eigen::MatrixXd dOuterM = getJacobianOfMinv(world, outerTau, wrt);

  snapshot.restore();

  return dt
         * (dOuterM
            + Minv * (-dC - A_c_ub_E * (dP_c + P_c * dt * (dM - Minv * dC))));
}
  */
}

//==============================================================================
const Eigen::MatrixXd& BackpropSnapshot::getPosPosJacobian(WorldPtr world)
{
  if (mCachedPosPosDirty)
  {

#ifdef USE_FD_OVERRIDE
    mCachedPosPos = finiteDifferencePosPosJacobian(world, 1);
#else
    RestorableSnapshot snapshot(world);
    world->setPositions(mPreStepPosition);
    world->setVelocities(mPreStepVelocity);
    world->setForces(mPreStepTorques);

    Eigen::MatrixXd A_b = getBouncingConstraintMatrix(world);

    // If there are no bounces, pos-pos is a simple identity
    if (A_b.size() == 0)
    {
      mCachedPosPos = Eigen::MatrixXd::Identity(mNumDOFs, mNumDOFs);
    }
    else
    {
      // Construct the W matrix we'll need to use to solve for our closest
      // approx
      Eigen::MatrixXd W = Eigen::MatrixXd(A_b.rows() * A_b.rows(), A_b.cols());
      for (int i = 0; i < A_b.cols(); i++)
      {
        Eigen::VectorXd a_i = A_b.col(i);
        for (int j = 0; j < A_b.rows(); j++)
        {
          W.block(j * A_b.rows(), i, A_b.rows(), 1) = a_i(j) * a_i;
        }
      }

      // We want to center the solution around the identity matrix, and find the
      // least-squares deviation along the diagonals that gets us there.
      Eigen::VectorXd center = Eigen::VectorXd::Zero(mNumDOFs * mNumDOFs);
      for (std::size_t i = 0; i < mNumDOFs; i++)
      {
        center((i * mNumDOFs) + i) = 1;
      }

      // Solve the linear system
      Eigen::VectorXd q
          = center
            - W.transpose().completeOrthogonalDecomposition().solve(
                getRestitutionDiagonals() + (W.eval().transpose() * center));

      // Recover X from the q vector
      Eigen::MatrixXd X = Eigen::MatrixXd(mNumDOFs, mNumDOFs);
      for (std::size_t i = 0; i < mNumDOFs; i++)
      {
        X.col(i) = q.segment(i * mNumDOFs, mNumDOFs);
      }

      mCachedPosPos = X;
    }
    snapshot.restore();
#endif

#ifdef SLOW_FD_CHECK_EVERYTHING
    // TODO: this is crappy, because if we are actually bouncing we want a
    // better approximation
    Eigen::MatrixXd bruteForce = finiteDifferencePosPosJacobian(world, 1);
    equalsOrCrash(world, mCachedPosPos, bruteForce, "pos-pos");
#endif

    mCachedPosPosDirty = false;
  }
  return mCachedPosPos;
}

//==============================================================================
const Eigen::MatrixXd& BackpropSnapshot::getVelPosJacobian(WorldPtr world)
{
  if (mCachedVelPosDirty)
  {
#ifdef USE_FD_OVERRIDE
    mCachedVelPos = finiteDifferenceVelPosJacobian(world, 1);
#else
    const Eigen::MatrixXd& posPos = getPosPosJacobian(world);
    mCachedVelPos = mTimeStep * posPos;
#endif

#ifdef SLOW_FD_CHECK_EVERYTHING
    // TODO: this is crappy, because if we are actually bouncing we want a
    // better approximation
    Eigen::MatrixXd bruteForce = finiteDifferenceVelPosJacobian(world, 1);
    equalsOrCrash(world, mCachedVelPos, bruteForce, "vel-pos");
#endif

    mCachedVelPosDirty = false;
  }
  return mCachedVelPos;
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getPreStepPosition()
{
  return mPreStepPosition;
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getPreStepVelocity()
{
  // return assembleVector<Eigen::VectorXd>(VectorToAssemble::PRE_STEP_VEL);
  return mPreStepVelocity;
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getPreStepTorques()
{
  // return assembleVector<Eigen::VectorXd>(VectorToAssemble::PRE_STEP_TAU);
  return mPreStepTorques;
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getPreConstraintVelocity()
{
  return mPreConstraintVelocities;
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getPostStepPosition()
{
  return mPostStepPosition;
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getPostStepVelocity()
{
  return mPostStepVelocity;
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getPostStepTorques()
{
  return mPostStepTorques;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getClampingConstraintMatrix(WorldPtr world)
{
  return assembleMatrix(world, MatrixToAssemble::CLAMPING);
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getMassedClampingConstraintMatrix(
    WorldPtr world)
{
  return assembleMatrix(world, MatrixToAssemble::MASSED_CLAMPING);
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getUpperBoundConstraintMatrix(WorldPtr world)
{
  return assembleMatrix(world, MatrixToAssemble::UPPER_BOUND);
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getMassedUpperBoundConstraintMatrix(
    WorldPtr world)
{
  return assembleMatrix(world, MatrixToAssemble::MASSED_UPPER_BOUND);
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getUpperBoundMappingMatrix()
{
  std::size_t numUpperBound = 0;
  std::size_t numClamping = 0;
  for (std::size_t i = 0; i < mGradientMatrices.size(); i++)
  {
    numUpperBound
        += mGradientMatrices[i]->getUpperBoundConstraintMatrix().cols();
    numClamping += mGradientMatrices[i]->getClampingConstraintMatrix().cols();
  }

  Eigen::MatrixXd mappingMatrix = Eigen::MatrixXd(numUpperBound, numClamping);
  mappingMatrix.setZero();

  std::size_t cursorUpperBound = 0;
  std::size_t cursorClamping = 0;
  for (std::size_t i = 0; i < mGradientMatrices.size(); i++)
  {
    Eigen::MatrixXd groupMappingMatrix
        = mGradientMatrices[i]->getUpperBoundMappingMatrix();
    mappingMatrix.block(
        cursorUpperBound,
        cursorClamping,
        groupMappingMatrix.rows(),
        groupMappingMatrix.cols())
        = groupMappingMatrix;

    cursorUpperBound += groupMappingMatrix.rows();
    cursorClamping += groupMappingMatrix.cols();
  }

  return mappingMatrix;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getBouncingConstraintMatrix(WorldPtr world)
{
  return assembleMatrix(world, MatrixToAssemble::BOUNCING);
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getMassMatrix(
    WorldPtr world, bool forFiniteDifferencing)
{
  return assembleBlockDiagonalMatrix(
      world,
      BackpropSnapshot::BlockDiagonalMatrixToAssemble::MASS,
      forFiniteDifferencing);
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getInvMassMatrix(
    WorldPtr world, bool forFiniteDifferencing)
{
  return assembleBlockDiagonalMatrix(
      world,
      BackpropSnapshot::BlockDiagonalMatrixToAssemble::INV_MASS,
      forFiniteDifferencing);
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getClampingAMatrix()
{
  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(mNumClamping, mNumClamping);
  int cursor = 0;
  for (int i = 0; i < mGradientMatrices.size(); i++)
  {
    int size = mGradientMatrices[i]->getClampingAMatrix().rows();
    result.block(cursor, cursor, size, size)
        = mGradientMatrices[i]->getClampingAMatrix();
    cursor += size;
  }
  return result;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getPosCJacobian(simulation::WorldPtr world)
{
  return assembleBlockDiagonalMatrix(
      world, BackpropSnapshot::BlockDiagonalMatrixToAssemble::POS_C);
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getVelCJacobian(simulation::WorldPtr world)
{
  return assembleBlockDiagonalMatrix(
      world, BackpropSnapshot::BlockDiagonalMatrixToAssemble::VEL_C);
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getContactConstraintImpluses()
{
  return assembleVector<Eigen::VectorXd>(
      VectorToAssemble::CONTACT_CONSTRAINT_IMPULSES);
}

//==============================================================================
Eigen::VectorXi BackpropSnapshot::getContactConstraintMappings()
{
  return assembleVector<Eigen::VectorXi>(
      VectorToAssemble::CONTACT_CONSTRAINT_MAPPINGS);
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getBounceDiagonals()
{
  return assembleVector<Eigen::VectorXd>(VectorToAssemble::BOUNCE_DIAGONALS);
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getRestitutionDiagonals()
{
  return assembleVector<Eigen::VectorXd>(
      VectorToAssemble::RESTITUTION_DIAGONALS);
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getPenetrationCorrectionVelocities()
{
  return assembleVector<Eigen::VectorXd>(
      VectorToAssemble::PENETRATION_VELOCITY_HACK);
}

//==============================================================================
/// Returns the constraint impulses along the clamping constraints
Eigen::VectorXd BackpropSnapshot::getClampingConstraintImpulses()
{
  return assembleVector<Eigen::VectorXd>(
      VectorToAssemble::CLAMPING_CONSTRAINT_IMPULSES);
}

//==============================================================================
/// Returns the relative velocities along the clamping constraints
Eigen::VectorXd BackpropSnapshot::getClampingConstraintRelativeVels()
{
  return assembleVector<Eigen::VectorXd>(
      VectorToAssemble::CLAMPING_CONSTRAINT_RELATIVE_VELS);
}

//==============================================================================
/// Returns the velocity change caused by illegal impulses in the LCP this
/// timestep
Eigen::VectorXd BackpropSnapshot::getVelocityDueToIllegalImpulses()
{
  return assembleVector<Eigen::VectorXd>(VectorToAssemble::VEL_DUE_TO_ILLEGAL);
}

//==============================================================================
/// Returns the coriolis and gravity forces pre-step
Eigen::VectorXd BackpropSnapshot::getCoriolisAndGravityForces()
{
  return assembleVector<Eigen::VectorXd>(
      VectorToAssemble::CORIOLIS_AND_GRAVITY);
}

//==============================================================================
/// Returns the velocity pre-LCP
Eigen::VectorXd BackpropSnapshot::getPreLCPVelocity()
{
  return assembleVector<Eigen::VectorXd>(VectorToAssemble::PRE_LCP_VEL);
}

//==============================================================================
bool BackpropSnapshot::hasBounces()
{
  return mNumBouncing > 0;
}

//==============================================================================
std::size_t BackpropSnapshot::getNumClamping()
{
  return mNumClamping;
}

//==============================================================================
std::size_t BackpropSnapshot::getNumUpperBound()
{
  return mNumUpperBound;
}

//==============================================================================
/// This is the clamping constraints from all the constrained
/// groups, concatenated together
std::vector<std::shared_ptr<DifferentiableContactConstraint>>
BackpropSnapshot::getDifferentiableConstraints()
{
  std::vector<std::shared_ptr<DifferentiableContactConstraint>> vec;
  vec.reserve(mNumConstraintDim);
  for (auto gradientMatrices : mGradientMatrices)
  {
    for (auto constraint : gradientMatrices->getDifferentiableConstraints())
    {
      vec.push_back(constraint);
    }
  }
  assert(vec.size() == mNumConstraintDim);
  return vec;
}

//==============================================================================
std::vector<std::shared_ptr<DifferentiableContactConstraint>>
BackpropSnapshot::getClampingConstraints()
{
  std::vector<std::shared_ptr<DifferentiableContactConstraint>> vec;
  vec.reserve(mNumClamping);
  for (auto gradientMatrices : mGradientMatrices)
  {
    for (auto constraint : gradientMatrices->getClampingConstraints())
    {
      constraint->setOffsetIntoWorld(vec.size(), false);
      vec.push_back(constraint);
    }
  }
  assert(vec.size() == mNumClamping);
  return vec;
}

//==============================================================================
std::vector<std::shared_ptr<DifferentiableContactConstraint>>
BackpropSnapshot::getUpperBoundConstraints()
{
  std::vector<std::shared_ptr<DifferentiableContactConstraint>> vec;
  vec.reserve(mNumUpperBound);
  for (auto gradientMatrices : mGradientMatrices)
  {
    for (auto constraint : gradientMatrices->getUpperBoundConstraints())
    {
      constraint->setOffsetIntoWorld(vec.size(), true);
      vec.push_back(constraint);
    }
  }
  return vec;
}

//==============================================================================
/// This verifies that the two matrices are equal to some tolerance, and if
/// they're not it prints the information needed to replicated this scenario
/// and it exits the program.
void BackpropSnapshot::equalsOrCrash(
    std::shared_ptr<simulation::World> world,
    Eigen::MatrixXd analytical,
    Eigen::MatrixXd bruteForce,
    std::string name)
{
  Eigen::MatrixXd diff = (analytical - bruteForce).cwiseAbs();
  double threshold = 1e-7;
  bool broken = (diff.array() > threshold).any();
  if (broken)
  {
    std::cout << "Found invalid matrix! " << name << std::endl;
    std::cout << "Analytical:" << std::endl << analytical << std::endl;
    std::cout << "Brute Force:" << std::endl << bruteForce << std::endl;
    std::cout << "Diff:" << std::endl << diff << std::endl;
    std::cout << "Code to replicate:" << std::endl;
    std::cout << "--------------------" << std::endl;
    std::cout << "Eigen::VectorXd brokenPos = Eigen::VectorXd::Zero("
              << mNumDOFs << ");" << std::endl;
    std::cout << "brokenPos <<" << std::endl;
    for (int i = 0; i < mNumDOFs; i++)
    {
      std::cout << "  " << mPreStepPosition(i);
      if (i == mNumDOFs - 1)
      {
        std::cout << ";" << std::endl;
      }
      else
      {
        std::cout << "," << std::endl;
      }
    }
    std::cout << "Eigen::VectorXd brokenVel = Eigen::VectorXd::Zero("
              << mNumDOFs << ");" << std::endl;
    std::cout << "brokenVel <<" << std::endl;
    for (int i = 0; i < mNumDOFs; i++)
    {
      std::cout << "  " << mPreStepVelocity(i);
      if (i == mNumDOFs - 1)
      {
        std::cout << ";" << std::endl;
      }
      else
      {
        std::cout << "," << std::endl;
      }
    }
    std::cout << "Eigen::VectorXd brokenForce = Eigen::VectorXd::Zero("
              << mNumDOFs << ");" << std::endl;
    std::cout << "brokenForce <<" << std::endl;
    for (int i = 0; i < mNumDOFs; i++)
    {
      std::cout << "  " << mPreStepTorques(i);
      if (i == mNumDOFs - 1)
      {
        std::cout << ";" << std::endl;
      }
      else
      {
        std::cout << "," << std::endl;
      }
    }
    std::cout << "world->setPositions(brokenPos);" << std::endl;
    std::cout << "world->setVelocities(brokenVel);" << std::endl;
    std::cout << "world->setForces(brokenForce);" << std::endl;
    std::cout << "--------------------" << std::endl;
    exit(1);
  }
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::finiteDifferenceVelVelJacobian(WorldPtr world)
{
  RestorableSnapshot snapshot(world);

  Eigen::MatrixXd J(mNumDOFs, mNumDOFs);

  bool oldGradientEnabled = world->getConstraintSolver()->getGradientEnabled();
  world->getConstraintSolver()->setGradientEnabled(false);

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);
  world->step(false);

  Eigen::VectorXd originalVel = world->getVelocities();

  double EPSILON = 1e-7;
  for (std::size_t i = 0; i < world->getNumDofs(); i++)
  {
    snapshot.restore();

    world->setPositions(mPreStepPosition);
    world->setForces(mPreStepTorques);
    Eigen::VectorXd tweakedVel = Eigen::VectorXd(mPreStepVelocity);
    tweakedVel(i) += EPSILON;
    world->setVelocities(tweakedVel);
    world->step(false);

    Eigen::VectorXd velPos = world->getVelocities();

    snapshot.restore();
    world->setPositions(mPreStepPosition);
    world->setForces(mPreStepTorques);
    tweakedVel = Eigen::VectorXd(mPreStepVelocity);
    tweakedVel(i) -= EPSILON;
    world->setVelocities(tweakedVel);
    world->step(false);

    Eigen::VectorXd velNeg = world->getVelocities();

    Eigen::VectorXd velChange = (velPos - velNeg) / (2 * EPSILON);
    J.col(i).noalias() = velChange;
  }

  snapshot.restore();
  world->getConstraintSolver()->setGradientEnabled(oldGradientEnabled);

  return J;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::finiteDifferencePosVelJacobian(
    simulation::WorldPtr world)
{
  RestorableSnapshot snapshot(world);

  Eigen::MatrixXd J(mNumDOFs, mNumDOFs);

  bool oldGradientEnabled = world->getConstraintSolver()->getGradientEnabled();
  // world->getConstraintSolver()->setGradientEnabled(false);
  bool oldPenetrationCorrectionEnabled
      = world->getConstraintSolver()->getPenetrationCorrectionEnabled();
  world->getConstraintSolver()->setPenetrationCorrectionEnabled(false);

  double dt = world->getTimeStep();

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  world->step(false);

  Eigen::VectorXd originalVel = world->getVelocities();

  double EPSILON = 1e-7;
  for (std::size_t i = 0; i < world->getNumDofs(); i++)
  {
    double eps = EPSILON;
    while (true)
    {
      // Get predicted next vel
      snapshot.restore();
      world->setForces(mPreStepTorques);
      world->setVelocities(mPreStepVelocity);
      Eigen::VectorXd tweakedPos = Eigen::VectorXd(mPreStepPosition);
      tweakedPos(i) += eps;
      world->setPositions(tweakedPos);

      Eigen::VectorXd preStepVel = world->getVelocities();
      BackpropSnapshotPtr ptr = neural::forwardPass(world, true);
      world->step(false);
      if (ptr->getNumClamping() == getNumClamping())
      {
        Eigen::VectorXd perturbedVel = world->getVelocities();
        J.col(i).noalias() = (perturbedVel - originalVel) / eps;
        break;
      }
      eps *= 0.5;
    }
  }

  snapshot.restore();
  world->getConstraintSolver()->setGradientEnabled(oldGradientEnabled);
  world->getConstraintSolver()->setPenetrationCorrectionEnabled(
      oldPenetrationCorrectionEnabled);

  return J;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::finiteDifferenceForceVelJacobian(
    WorldPtr world)
{
  RestorableSnapshot snapshot(world);

  Eigen::MatrixXd J(mNumDOFs, mNumDOFs);

  bool oldGradientEnabled = world->getConstraintSolver()->getGradientEnabled();
  world->getConstraintSolver()->setGradientEnabled(false);

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);
  world->step(false);

  Eigen::VectorXd originalForces = world->getForces();
  Eigen::VectorXd originalVel = world->getVelocities();

  double EPSILON = 1e-7;
  for (std::size_t i = 0; i < world->getNumDofs(); i++)
  {
    snapshot.restore();

    world->setPositions(mPreStepPosition);
    world->setVelocities(mPreStepVelocity);
    Eigen::VectorXd tweakedForces = Eigen::VectorXd(originalForces);
    tweakedForces(i) += EPSILON;
    world->setForces(tweakedForces);

    world->step(false);

    Eigen::VectorXd velChange
        = (world->getVelocities() - originalVel) / EPSILON;
    J.col(i).noalias() = velChange;
  }

  snapshot.restore();
  world->getConstraintSolver()->setGradientEnabled(oldGradientEnabled);

  return J;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::finiteDifferencePosPosJacobian(
    WorldPtr world, std::size_t subdivisions)
{
  RestorableSnapshot snapshot(world);

  double oldTimestep = world->getTimeStep();
  world->setTimeStep(oldTimestep / subdivisions);
  bool oldGradientEnabled = world->getConstraintSolver()->getGradientEnabled();
  world->getConstraintSolver()->setGradientEnabled(false);

  Eigen::MatrixXd J(mNumDOFs, mNumDOFs);

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  for (std::size_t j = 0; j < subdivisions; j++)
    world->step(false);

  Eigen::VectorXd originalPosition = world->getPositions();

  // IMPORTANT: EPSILON must be larger than the distance traveled in a single
  // subdivided timestep. Ideally much larger.
  double EPSILON = 1e-2 / subdivisions;
  for (std::size_t i = 0; i < world->getNumDofs(); i++)
  {
    snapshot.restore();

    world->setVelocities(mPreStepVelocity);
    world->setForces(mPreStepTorques);

    Eigen::VectorXd tweakedPositions = Eigen::VectorXd(mPreStepPosition);
    tweakedPositions(i) += EPSILON;
    world->setPositions(tweakedPositions);

    for (std::size_t j = 0; j < subdivisions; j++)
      world->step(false);

    Eigen::VectorXd posChange
        = (world->getPositions() - originalPosition) / EPSILON;
    J.col(i).noalias() = posChange;
  }

  world->setTimeStep(oldTimestep);
  world->getConstraintSolver()->setGradientEnabled(oldGradientEnabled);
  snapshot.restore();

  return J;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::finiteDifferenceVelPosJacobian(
    WorldPtr world, std::size_t subdivisions)
{
  RestorableSnapshot snapshot(world);

  double oldTimestep = world->getTimeStep();
  world->setTimeStep(oldTimestep / subdivisions);
  bool oldGradientEnabled = world->getConstraintSolver()->getGradientEnabled();
  world->getConstraintSolver()->setGradientEnabled(false);

  Eigen::MatrixXd J(mNumDOFs, mNumDOFs);

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  for (std::size_t j = 0; j < subdivisions; j++)
    world->step(false);

  Eigen::VectorXd originalPosition = world->getPositions();

  double EPSILON = 1e-3 / subdivisions;
  for (std::size_t i = 0; i < world->getNumDofs(); i++)
  {
    snapshot.restore();

    world->setPositions(mPreStepPosition);
    world->setForces(mPreStepTorques);

    Eigen::VectorXd tweakedVelocity = Eigen::VectorXd(mPreStepVelocity);
    tweakedVelocity(i) += EPSILON;
    world->setVelocities(tweakedVelocity);

    for (std::size_t j = 0; j < subdivisions; j++)
      world->step(false);

    Eigen::VectorXd posChange
        = (world->getPositions() - originalPosition) / EPSILON;
    J.col(i).noalias() = posChange;
  }

  world->setTimeStep(oldTimestep);
  world->getConstraintSolver()->setGradientEnabled(oldGradientEnabled);
  snapshot.restore();

  return J;
}

/*
//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getProjectionIntoClampsMatrix(
    WorldPtr world, bool forFiniteDifferencing)
{
  Eigen::MatrixXd A_c;
  if (forFiniteDifferencing)
  {
    A_c = getClampingConstraintMatrixAt(world, world->getPositions());
  }
  else
  {
    A_c = getClampingConstraintMatrix(world);
  }
  if (A_c.size() == 0)
    return Eigen::MatrixXd::Zero(0, world->getNumDofs());

  Eigen::MatrixXd E = getUpperBoundMappingMatrix();

  Eigen::MatrixXd constraintForceToImpliedTorques;
  if (forFiniteDifferencing || true)
  {
    Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
    Eigen::MatrixXd Minv = getInvMassMatrix(world, forFiniteDifferencing);
    constraintForceToImpliedTorques = Minv * (A_c + (A_ub * E));
  }
  else
  {
    Eigen::MatrixXd V_c = getMassedClampingConstraintMatrix(world);
    Eigen::MatrixXd V_ub = getMassedUpperBoundConstraintMatrix(world);
    constraintForceToImpliedTorques = V_c + (V_ub * E);
  }

  Eigen::MatrixXd forceToVel
      = A_c.eval().transpose() * constraintForceToImpliedTorques;
  Eigen::MatrixXd bounce = getBounceDiagonals().asDiagonal();
  Eigen::MatrixXd rightHandSize = bounce * A_c.transpose();
  return (1.0 / mTimeStep)
         * forceToVel.completeOrthogonalDecomposition().solve(rightHandSize);
}
*/

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getProjectionIntoClampsMatrix(
    WorldPtr world, bool forFiniteDifferencing)
{
  Eigen::MatrixXd A_c;
  if (forFiniteDifferencing)
  {
    A_c = getClampingConstraintMatrixAt(world, world->getPositions());
  }
  else
  {
    A_c = getClampingConstraintMatrix(world);
  }
  if (A_c.size() == 0)
    return Eigen::MatrixXd::Zero(0, world->getNumDofs());

  Eigen::MatrixXd constraintForceToImpliedTorques;
  if (forFiniteDifferencing)
  {
    Eigen::MatrixXd A_ub
        = getUpperBoundConstraintMatrixAt(world, world->getPositions());
    Eigen::MatrixXd E
        = getUpperBoundMappingMatrixAt(world, world->getPositions());
    Eigen::MatrixXd Minv = getInvMassMatrix(world, true);
    constraintForceToImpliedTorques = Minv * (A_c + (A_ub * E));

    Eigen::MatrixXd forceToVel
        = A_c.eval().transpose() * constraintForceToImpliedTorques;
    Eigen::MatrixXd bounce
        = getBounceDiagonalsAt(world, world->getPositions()).asDiagonal();
    Eigen::MatrixXd rightHandSize = bounce * A_c.transpose();
    return (1.0 / mTimeStep)
           * forceToVel.completeOrthogonalDecomposition().solve(rightHandSize);
  }
  else
  {
    Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
    Eigen::MatrixXd E = getUpperBoundMappingMatrix();
    Eigen::MatrixXd Minv = getInvMassMatrix(world, false);
    constraintForceToImpliedTorques = Minv * (A_c + (A_ub * E));
    // We don't use the massed formulation anymore because it introduces slight
    // numerical instability

    // Eigen::MatrixXd V_c = getMassedClampingConstraintMatrix(world);
    // Eigen::MatrixXd V_ub = getMassedUpperBoundConstraintMatrix(world);
    // constraintForceToImpliedTorques = V_c + (V_ub * E);
    Eigen::MatrixXd forceToVel
        = A_c.eval().transpose() * constraintForceToImpliedTorques;
    Eigen::MatrixXd bounce = getBounceDiagonals().asDiagonal();
    Eigen::MatrixXd rightHandSize = bounce * A_c.transpose();
    return (1.0 / mTimeStep)
           * forceToVel.completeOrthogonalDecomposition().solve(rightHandSize);
  }
}

/// This returns the result of M*x, without explicitly
/// forming M
Eigen::VectorXd BackpropSnapshot::implicitMultiplyByMassMatrix(
    simulation::WorldPtr world, const Eigen::VectorXd& x)
{
  Eigen::VectorXd result = x;
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < world->getNumSkeletons(); i++)
  {
    SkeletonPtr skel = world->getSkeleton(i);
    std::size_t dofs = skel->getNumDofs();
    result.segment(cursor, dofs)
        = skel->multiplyByImplicitMassMatrix(x.segment(cursor, dofs));
    cursor += dofs;
  }
  return result;
}

/// This return the result of Minv*x, without explicitly
/// forming Minv
Eigen::VectorXd BackpropSnapshot::implicitMultiplyByInvMassMatrix(
    simulation::WorldPtr world, const Eigen::VectorXd& x)
{
  Eigen::VectorXd result = x;
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < world->getNumSkeletons(); i++)
  {
    SkeletonPtr skel = world->getSkeleton(i);
    std::size_t dofs = skel->getNumDofs();
    result.segment(cursor, dofs)
        = skel->multiplyByImplicitInvMassMatrix(x.segment(cursor, dofs));
    cursor += dofs;
  }
  return result;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::getJacobianOfConstraintForce(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  if (A_c.cols() == 0)
  {
    int wrtDim = getWrtDim(world, wrt);
    return Eigen::MatrixXd::Zero(0, wrtDim);
  }

  /*
    RestorableSnapshot snapshot(world);
    world->setPositions(mPreStepPosition);
    world->setVelocities(mPreStepVelocity);
    world->setForces(mPreStepTorques);
    */

  Eigen::MatrixXd Q = getClampingAMatrix();

  Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> Qfac
      = Q.completeOrthogonalDecomposition();

  Eigen::MatrixXd dB = getJacobianOfLCPOffsetClampingSubset(world, wrt);

  if (wrt == WithRespectTo::VELOCITY || wrt == WithRespectTo::FORCE)
  {
    // dQ_b is 0, so don't compute it
    return Qfac.solve(dB);
  }

  Eigen::VectorXd b = getClampingConstraintRelativeVels();
  Eigen::MatrixXd dQ_b
      = getJacobianOfLCPConstraintMatrixClampingSubset(world, b, wrt);

  // snapshot.restore();

  return dQ_b + Qfac.solve(dB);
}

//==============================================================================
Eigen::MatrixXd
BackpropSnapshot::getJacobianOfLCPConstraintMatrixClampingSubset(
    simulation::WorldPtr world, Eigen::VectorXd b, WithRespectTo wrt)
{
  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  if (A_c.cols() == 0)
  {
    return Eigen::MatrixXd::Zero(0, 0);
  }
  if (wrt == VELOCITY || wrt == FORCE)
  {
    return Eigen::MatrixXd::Zero(A_c.cols(), A_c.cols());
  }

  Eigen::MatrixXd Minv = getInvMassMatrix(world);
  Eigen::MatrixXd Q = getClampingAMatrix(); // A_c.transpose() * Minv * A_c;
  Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> Qfactored
      = Q.completeOrthogonalDecomposition();

  Eigen::VectorXd Qinv_b = Qfactored.solve(b);

  if (wrt == POSITION)
  {
    // Position is the only term that affects A_c
    Eigen::MatrixXd innerTerms
        = getJacobianOfClampingConstraintsTranspose(world, Minv * A_c * Qinv_b)
          + A_c.transpose() * getJacobianOfMinv(world, A_c * Qinv_b, wrt)
          + A_c.transpose() * Minv
                * getJacobianOfClampingConstraints(world, Qinv_b);
    Eigen::MatrixXd result = -Qfactored.solve(innerTerms);
    return result;
  }
  else
  {
    // All other terms get to treat A_c as constant
    Eigen::MatrixXd innerTerms
        = A_c.transpose() * getJacobianOfMinv(world, A_c * Qinv_b, wrt);
    Eigen::MatrixXd result = -Qfactored.solve(innerTerms);
    return result;
  }
}

//==============================================================================
/// This returns the jacobian of b (from Q^{-1}b) with respect to wrt
Eigen::MatrixXd BackpropSnapshot::getJacobianOfLCPOffsetClampingSubset(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  double dt = world->getTimeStep();
  Eigen::MatrixXd Minv = getInvMassMatrix(world);
  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  Eigen::MatrixXd dC = getJacobianOfC(world, wrt);
  if (wrt == WithRespectTo::VELOCITY)
  {
    return -A_c.transpose()
           * (Eigen::MatrixXd::Identity(
                  world->getNumDofs(), world->getNumDofs())
              + dt * Minv * dC);
  }
  else if (wrt == WithRespectTo::FORCE)
  {
    return -A_c.transpose() * dt * Minv;
  }

  Eigen::VectorXd C = getCoriolisAndGravityForces();
  Eigen::VectorXd f = getPreStepTorques() - C;
  Eigen::MatrixXd dMinv_f = getJacobianOfMinv(world, f, wrt);
  Eigen::VectorXd v_f = getPreConstraintVelocity();

  /*
  std::cout << "Minv: " << std::endl << Minv << std::endl;
  std::cout << "dC: " << std::endl << dC << std::endl;
  std::cout << "Minv * dC: " << std::endl << Minv * dC << std::endl;
  std::cout << "A_c.transpose(): " << std::endl << A_c.transpose() << std::endl;
  std::cout << "A_c.transpose() * Minv * dC: " << std::endl
            << A_c.transpose() * Minv * dC << std::endl;
  */

  if (wrt == WithRespectTo::POSITION)
  {
    Eigen::MatrixXd dA_c_f
        = getJacobianOfClampingConstraintsTranspose(world, v_f);

    return -(dA_c_f + A_c.transpose() * dt * (dMinv_f - Minv * dC));
  }
  else
  {
    return -(A_c.transpose() * dt * (dMinv_f - Minv * dC));
  }
}

//==============================================================================
/// This returns the subset of the A matrix used by the original LCP for just
/// the clamping constraints. It relates constraint force to constraint
/// acceleration. It's a mass matrix, just in a weird frame.
void BackpropSnapshot::computeLCPConstraintMatrixClampingSubset(
    simulation::WorldPtr world, Eigen::MatrixXd& Q, const Eigen::MatrixXd& A_c)
{
  /*
  int numClamping = A_c.cols();
  for (int i = 0; i < numClamping; i++)
  {
    Q.col(i)
        = A_c.transpose() * implicitMultiplyByInvMassMatrix(world, A_c.col(i));
  }
  */
  Q = A_c.transpose() * getInvMassMatrix(world) * A_c;
}

//==============================================================================
/// This returns the subset of the b vector used by the original LCP for just
/// the clamping constraints. It's just the relative velocity at the clamping
/// contact points.
void BackpropSnapshot::computeLCPOffsetClampingSubset(
    simulation::WorldPtr world, Eigen::VectorXd& b, const Eigen::MatrixXd& A_c)
{
  /*
  Eigen::VectorXd velDiff = world->getVelocities() - mPreStepVelocity;
  b = getClampingConstraintRelativeVels() + A_c.transpose() * velDiff;
  */

  /*
  b = -A_c.transpose()
      * (world->getVelocities() + getVelocityDueToIllegalImpulses());
  */

  /*
  b = -A_c.transpose()
      * (getPreConstraintVelocity() + getVelocityDueToIllegalImpulses());
  */
  b = -getBounceDiagonals().cwiseProduct(
      A_c.transpose()
      * (world->getVelocities()
         + (world->getTimeStep()
            * implicitMultiplyByInvMassMatrix(
                world,
                world->getForces()
                    - world->getCoriolisAndGravityAndExternalForces()))));
}

//==============================================================================
/// This computes and returns an estimate of the constraint impulses for the
/// clamping constraints. This is based on a linear approximation of the
/// constraint impulses.
Eigen::VectorXd BackpropSnapshot::estimateClampingConstraintImpulses(
    simulation::WorldPtr world, const Eigen::MatrixXd& A_c)
{
  if (A_c.cols() == 0)
  {
    return Eigen::VectorXd::Zero(0);
  }

  Eigen::VectorXd b = Eigen::VectorXd(A_c.cols());
  Eigen::MatrixXd Q = Eigen::MatrixXd(A_c.cols(), A_c.cols());
  computeLCPOffsetClampingSubset(world, b, A_c);
  computeLCPConstraintMatrixClampingSubset(world, Q, A_c);

  return Q.completeOrthogonalDecomposition().solve(b);
}

//==============================================================================
/// This returns the jacobian of P_c * v, holding everyhing constant except
/// the value of WithRespectTo
Eigen::MatrixXd BackpropSnapshot::getJacobianOfProjectionIntoClampsMatrix(
    simulation::WorldPtr world, Eigen::VectorXd v, WithRespectTo wrt)
{
  // return finiteDifferenceJacobianOfProjectionIntoClampsMatrix(world, v, wrt);

  Eigen::MatrixXd A_c = getClampingConstraintMatrix(world);
  if (A_c.size() == 0)
    return Eigen::MatrixXd::Zero(0, world->getNumDofs());
  Eigen::MatrixXd A_ub = getUpperBoundConstraintMatrix(world);
  Eigen::MatrixXd E = getUpperBoundMappingMatrix();

  Eigen::MatrixXd V_c = getMassedClampingConstraintMatrix(world);
  Eigen::MatrixXd V_ub = getMassedUpperBoundConstraintMatrix(world);
  Eigen::MatrixXd constraintForceToImpliedTorques = V_c + (V_ub * E);
  Eigen::MatrixXd A_c_ub_E = A_c + (A_ub * E);
  Eigen::MatrixXd Q = A_c.eval().transpose() * constraintForceToImpliedTorques;
  auto XFactor = Q.completeOrthogonalDecomposition();
  Eigen::MatrixXd bounce = getBounceDiagonals().asDiagonal();

  // New formulation
  if (wrt == POSITION)
  {
    // d/d Q^{-1} v = - Q^{-1} (d/d Q) Q^{-1} v
    Eigen::MatrixXd rightHandSide = bounce * A_c.transpose();
    Eigen::MatrixXd dRhs
        = bounce * getJacobianOfClampingConstraintsTranspose(world, v);
    Eigen::MatrixXd Minv = getInvMassMatrix(world);

    Eigen::MatrixXd Qinv = XFactor.pseudoInverse();
    Eigen::VectorXd Qinv_v = XFactor.solve(rightHandSide * v);
    Eigen::MatrixXd dQ
        = getJacobianOfClampingConstraintsTranspose(
              world, Minv * A_c_ub_E * Qinv_v)
          + A_c.transpose()
                * (getJacobianOfMinv(world, A_c_ub_E * Qinv_v, POSITION)
                   + Minv * getJacobianOfClampingConstraints(world, Qinv_v));

    return (1 / world->getTimeStep())
           * (XFactor.solve(dRhs) - XFactor.solve(dQ));
  }
  else
  {
    // Ignore changes to A_c

    // Approximate the pseudo-inverse as just a plain inverse for the purposes
    // of derivation

    Eigen::VectorXd tau
        = A_c_ub_E * XFactor.solve(bounce * A_c.transpose() * v);

    Eigen::MatrixXd MinvJac = getJacobianOfMinv(world, tau, wrt);

    return -(1.0 / world->getTimeStep())
           * XFactor.solve(A_c.transpose() * MinvJac);
  }

  // An older approach that attempted to handle pseudoinverse distinct from
  // normal inverse

  /*
  Eigen::MatrixXd X = XFactor.pseudoInverse();
  Eigen::VectorXd A_c_T_V = bounce * A_c.transpose() * v;

  // Part 1

  Eigen::VectorXd part1Tau = A_c * X.transpose() * X * A_c_T_V;
  Eigen::MatrixXd part1MinvJac = getJacobianOfMinv(world, part1Tau, wrt);
  Eigen::MatrixXd XQ = X * Q;
  Eigen::MatrixXd part1 = (Eigen::MatrixXd::Identity(XQ.rows(), XQ.cols()) + XQ)
                          * A_c_ub_E.transpose() * part1MinvJac;

  Eigen::MatrixXd QX = Q * X;
  Eigen::VectorXd part2Tau
      = A_c * (Eigen::MatrixXd::Identity(QX.rows(), QX.cols()) - QX) * A_c_T_V;
  Eigen::MatrixXd part2MinvJac = getJacobianOfMinv(world, part2Tau, wrt);
  Eigen::MatrixXd part2
      = X * X.transpose() * A_c_ub_E.transpose() * part2MinvJac;

  Eigen::VectorXd part3Tau = A_c_ub_E * X * A_c_T_V;
  Eigen::MatrixXd part3MinvJac = getJacobianOfMinv(world, part3Tau, wrt);
  Eigen::MatrixXd part3 = X * A_c.transpose() * part3MinvJac;

  return (1.0 / mTimeStep) * (part1 + part2 - part3);
  */
}

//==============================================================================
/// This returns the jacobian of M^{-1}(pos, inertia) * tau, holding
/// everything constant except the value of WithRespectTo
Eigen::MatrixXd BackpropSnapshot::getJacobianOfMinv(
    simulation::WorldPtr world, Eigen::VectorXd tau, WithRespectTo wrt)
{
  return finiteDifferenceJacobianOfMinv(world, tau, wrt);
}

//==============================================================================
/// This returns the jacobian of C(pos, inertia, vel), holding everything
/// constant except the value of WithRespectTo
Eigen::MatrixXd BackpropSnapshot::getJacobianOfC(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  return finiteDifferenceJacobianOfC(world, wrt);
}

/// This returns the jacobian of M^{-1}(pos, inertia) * (C(pos, inertia, vel) +
/// mPreStepTorques), holding everything constant except the value of
/// WithRespectTo
Eigen::MatrixXd BackpropSnapshot::getJacobianOfMinvC(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  return finiteDifferenceJacobianOfMinvC(world, wrt);
}

//==============================================================================
/// This returns a fast approximation to A_c in the neighborhood of the original
Eigen::MatrixXd BackpropSnapshot::estimateClampingConstraintMatrixAt(
    simulation::WorldPtr world, Eigen::VectorXd pos)
{
  Eigen::VectorXd posDiff = pos - mPreStepPosition;
  if (posDiff.squaredNorm() == 0)
  {
    return getClampingConstraintMatrix(world);
  }
  auto clampingConstraints = getClampingConstraints();
  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(mNumDOFs, mNumClamping);
  for (int i = 0; i < clampingConstraints.size(); i++)
  {
    auto constraint = clampingConstraints[i];
    result.col(i) = constraint->getConstraintForces(world)
                    + constraint->getConstraintForcesJacobian(world) * posDiff;
  }
  return result;
}

//==============================================================================
/// This returns a fast approximation to A_ub in the neighborhood of the
/// original
Eigen::MatrixXd BackpropSnapshot::estimateUpperBoundConstraintMatrixAt(
    simulation::WorldPtr world, Eigen::VectorXd pos)
{
  Eigen::VectorXd posDiff = pos - mPreStepPosition;
  if (posDiff.squaredNorm() == 0)
  {
    return getUpperBoundConstraintMatrix(world);
  }
  auto upperBoundConstraints = getUpperBoundConstraints();
  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(mNumDOFs, mNumUpperBound);
  for (int i = 0; i < upperBoundConstraints.size(); i++)
  {
    auto constraint = upperBoundConstraints[i];
    result.col(i) = constraint->getConstraintForces(world)
                    + constraint->getConstraintForcesJacobian(world) * posDiff;
  }
  return result;
}

//==============================================================================
/// Only for testing: VERY SLOW. This returns the actual value of A_c at the
/// desired position.
Eigen::MatrixXd BackpropSnapshot::getClampingConstraintMatrixAt(
    simulation::WorldPtr world, Eigen::VectorXd pos)
{
  RestorableSnapshot snapshot(world);
  world->setPositions(pos);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  BackpropSnapshotPtr ptr = neural::forwardPass(world, true);

  snapshot.restore();

  Eigen::MatrixXd bruteResult = ptr->getClampingConstraintMatrix(world);
  return bruteResult;
}

//==============================================================================
/// Only for testing: VERY SLOW. This returns the actual value of A_ub at the
/// desired position.
Eigen::MatrixXd BackpropSnapshot::getUpperBoundConstraintMatrixAt(
    simulation::WorldPtr world, Eigen::VectorXd pos)
{
  RestorableSnapshot snapshot(world);
  world->setPositions(pos);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  BackpropSnapshotPtr ptr = neural::forwardPass(world, true);

  snapshot.restore();

  return ptr->getUpperBoundConstraintMatrix(world);
}

//==============================================================================
/// Only for testing: VERY SLOW. This returns the actual value of E at the
/// desired position.
Eigen::MatrixXd BackpropSnapshot::getUpperBoundMappingMatrixAt(
    simulation::WorldPtr world, Eigen::VectorXd pos)
{
  RestorableSnapshot snapshot(world);
  world->setPositions(pos);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  BackpropSnapshotPtr ptr = neural::forwardPass(world, true);

  snapshot.restore();

  return ptr->getUpperBoundMappingMatrix();
}

//==============================================================================
/// Only for testing: VERY SLOW. This returns the actual value of the bounce
/// diagonals at the desired position.
Eigen::VectorXd BackpropSnapshot::getBounceDiagonalsAt(
    simulation::WorldPtr world, Eigen::VectorXd pos)
{
  RestorableSnapshot snapshot(world);
  world->setPositions(pos);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  BackpropSnapshotPtr ptr = neural::forwardPass(world, true);

  snapshot.restore();

  return ptr->getBounceDiagonals();
}

//==============================================================================
/// This computes the Jacobian of A_c*f0 with respect to `wrt` using impulse
/// tests.
Eigen::MatrixXd BackpropSnapshot::getJacobianOfClampingConstraints(
    simulation::WorldPtr world, Eigen::VectorXd f0)
{
  std::vector<std::shared_ptr<DifferentiableContactConstraint>> constraints
      = getClampingConstraints();
  int dofs = world->getNumDofs();
  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(dofs, dofs);
  assert(constraints.size() == f0.size());
  for (int i = 0; i < constraints.size(); i++)
  {
    result += f0(i) * constraints[i]->getConstraintForcesJacobian(world);
  }
  return result;
}

//==============================================================================
/// This computes the Jacobian of A_c^T*v0 with respect to position using
/// impulse tests.
Eigen::MatrixXd BackpropSnapshot::getJacobianOfClampingConstraintsTranspose(
    simulation::WorldPtr world, Eigen::VectorXd v0)
{
  std::vector<std::shared_ptr<DifferentiableContactConstraint>> constraints
      = getClampingConstraints();
  int dofs = world->getNumDofs();
  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(mNumClamping, dofs);
  for (int i = 0; i < constraints.size(); i++)
  {
    result.row(i)
        = constraints[i]->getConstraintForcesJacobian(world).transpose() * v0;
  }

  return result;
}

//==============================================================================
/// This computes the Jacobian of A_ub*E*f0 with respect to position using
/// impulse tests.
Eigen::MatrixXd BackpropSnapshot::getJacobianOfUpperBoundConstraints(
    simulation::WorldPtr world, Eigen::VectorXd f0)
{
  std::vector<std::shared_ptr<DifferentiableContactConstraint>> constraints
      = getUpperBoundConstraints();
  int dofs = world->getNumDofs();
  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(dofs, dofs);
  assert(constraints.size() == f0.size());
  for (int i = 0; i < constraints.size(); i++)
  {
    result += f0(i) * constraints[i]->getConstraintForcesJacobian(world);
  }
  return result;
}

//==============================================================================
/// This measures a vector of contact impulses (measured at the clamping
/// constraints) on the world, to see what total velocity change results. This
/// is a fast way to get A_c * f0.
Eigen::VectorXd BackpropSnapshot::getClampingImpulseVelChange(
    simulation::WorldPtr world, Eigen::VectorXd f0)
{
  std::vector<std::shared_ptr<DifferentiableContactConstraint>>
      clampingConstraints = getClampingConstraints();
  assert(
      clampingConstraints.size() == f0.size()
      && "f0 must have exactly one entry per clamping constraint");
  for (int i = 0; i < world->getNumSkeletons(); i++)
  {
    auto skel = world->getSkeleton(i);
    skel->clearConstraintImpulses();
  }
  for (int i = 0; i < f0.size(); i++)
  {
    // TODO(keenon): Finish implementing me if useful
    // clampingConstraints[i]->applyImpulse(f0(i));
  }
}

//==============================================================================
/// This computes the finite difference Jacobian of A_c*f0 with respect to
/// position
Eigen::MatrixXd BackpropSnapshot::finiteDifferenceJacobianOfClampingConstraints(
    simulation::WorldPtr world, Eigen::VectorXd f0)
{
  RestorableSnapshot snapshot(world);

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  Eigen::VectorXd original = getClampingConstraintMatrix(world) * f0;

  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(original.size(), mNumDOFs);

  const double EPS = 1e-8;

  for (std::size_t i = 0; i < mNumDOFs; i++)
  {
    double posEPS = EPS;
    Eigen::VectorXd perturbedResultPos;
    // Keep scaling down the EPS until it no longer results in a different
    // number of columns
    for (int j = 0; j < 10; j++)
    {
      snapshot.restore();
      Eigen::VectorXd perturbed = mPreStepPosition;
      perturbed(i) += posEPS;
      world->setPositions(perturbed);
      world->setVelocities(mPreStepVelocity);
      world->setForces(mPreStepTorques);

      BackpropSnapshotPtr ptr = neural::forwardPass(world, true);
      Eigen::MatrixXd perturbedA_c = ptr->getClampingConstraintMatrix(world);
      if (perturbedA_c.cols() == f0.size())
      {
        perturbedResultPos = perturbedA_c * f0;
        break;
      }
      posEPS /= 2;
    }

    double negEPS = EPS;
    Eigen::VectorXd perturbedResultNeg;
    // Keep scaling down the EPS until it no longer results in a different
    // number of columns
    for (int j = 0; j < 10; j++)
    {
      snapshot.restore();
      Eigen::VectorXd perturbed = mPreStepPosition;
      perturbed(i) -= negEPS;
      world->setPositions(perturbed);
      world->setVelocities(mPreStepVelocity);
      world->setForces(mPreStepTorques);

      BackpropSnapshotPtr ptr = neural::forwardPass(world, true);
      Eigen::MatrixXd perturbedA_c = ptr->getClampingConstraintMatrix(world);
      if (perturbedA_c.cols() == f0.size())
      {
        perturbedResultNeg = perturbedA_c * f0;
        break;
      }
      negEPS /= 2;
    }

    result.col(i)
        = (perturbedResultPos - perturbedResultNeg) / (posEPS + negEPS);
  }

  snapshot.restore();

  return result;
}

//==============================================================================
/// This computes the finite difference Jacobian of A_c^T*v0 with respect to
/// position. This is AS SLOW AS FINITE DIFFERENCING THE WHOLE ENGINE, which
/// is way too slow to use in practice.
Eigen::MatrixXd
BackpropSnapshot::finiteDifferenceJacobianOfClampingConstraintsTranspose(
    simulation::WorldPtr world, Eigen::VectorXd v0)
{
  RestorableSnapshot snapshot(world);

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  Eigen::VectorXd original
      = getClampingConstraintMatrix(world).transpose() * v0;

  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(original.size(), mNumDOFs);

  const double EPS = 1e-8;

  for (std::size_t i = 0; i < mNumDOFs; i++)
  {
    double posEPS = EPS;
    Eigen::VectorXd perturbedResultPos;

    // Keep scaling down the EPS until it no longer results in a different
    // number of columns
    for (int j = 0; j < 10; j++)
    {
      snapshot.restore();
      Eigen::VectorXd perturbed = mPreStepPosition;
      perturbed(i) += posEPS;
      world->setPositions(perturbed);
      world->setVelocities(mPreStepVelocity);
      world->setForces(mPreStepTorques);

      BackpropSnapshotPtr ptr = neural::forwardPass(world, true);
      perturbedResultPos
          = ptr->getClampingConstraintMatrix(world).transpose() * v0;
      if (perturbedResultPos.size() == original.size())
      {
        break;
      }
      posEPS /= 2;
    }

    double negEPS = EPS;
    Eigen::VectorXd perturbedResultNeg;

    // Keep scaling down the EPS until it no longer results in a different
    // number of columns
    for (int j = 0; j < 10; j++)
    {
      snapshot.restore();
      Eigen::VectorXd perturbed = mPreStepPosition;
      perturbed(i) -= negEPS;
      world->setPositions(perturbed);
      world->setVelocities(mPreStepVelocity);
      world->setForces(mPreStepTorques);

      BackpropSnapshotPtr ptr = neural::forwardPass(world, true);
      perturbedResultNeg
          = ptr->getClampingConstraintMatrix(world).transpose() * v0;
      if (perturbedResultNeg.size() == original.size())
      {
        break;
      }
      negEPS /= 2;
    }

    result.col(i)
        = (perturbedResultPos - perturbedResultNeg) / (posEPS + negEPS);
  }

  snapshot.restore();

  return result;
}

/// This computes the finite difference Jacobian of A_ub*E*f0 with respect to
/// position. This is AS SLOW AS FINITE DIFFERENCING THE WHOLE ENGINE, which
/// is way too slow to use in practice.
Eigen::MatrixXd
BackpropSnapshot::finiteDifferenceJacobianOfUpperBoundConstraints(
    simulation::WorldPtr world, Eigen::VectorXd f0)
{
  if (mNumUpperBound == 0)
  {
    return Eigen::MatrixXd::Zero(mNumDOFs, mNumDOFs);
  }

  RestorableSnapshot snapshot(world);

  world->setPositions(mPreStepPosition);
  world->setVelocities(mPreStepVelocity);
  world->setForces(mPreStepTorques);

  Eigen::MatrixXd E = getUpperBoundMappingMatrix();

  Eigen::VectorXd original = getUpperBoundConstraintMatrix(world) * E * f0;

  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(mNumUpperBound, mNumDOFs);

  const double EPS = 1e-8;

  for (std::size_t i = 0; i < mNumDOFs; i++)
  {
    snapshot.restore();
    Eigen::VectorXd perturbed = mPreStepPosition;
    perturbed(i) += EPS;
    world->setPositions(perturbed);
    world->setVelocities(mPreStepVelocity);
    world->setForces(mPreStepTorques);

    BackpropSnapshotPtr ptr = neural::forwardPass(world, true);
    Eigen::VectorXd perturbedResult
        = ptr->getUpperBoundConstraintMatrix(world) * E * f0;
    result.col(i) = (perturbedResult - original) / EPS;
  }

  snapshot.restore();

  return result;
}

//==============================================================================
/// This computes and returns the jacobian of P_c * v by finite
/// differences. This is SUPER SLOW, and is only here for testing.
Eigen::MatrixXd
BackpropSnapshot::finiteDifferenceJacobianOfProjectionIntoClampsMatrix(
    simulation::WorldPtr world, Eigen::VectorXd v, WithRespectTo wrt)
{
  std::size_t innerDim = getWrtDim(world, wrt);

  Eigen::VectorXd before = getWrt(world, wrt);

  // These are predicted contact forces at the clamping contacts
  Eigen::VectorXd original = getProjectionIntoClampsMatrix(world, true) * v;

  Eigen::MatrixXd originalP_c = getProjectionIntoClampsMatrix(world, true);

  std::vector<std::shared_ptr<DifferentiableContactConstraint>> constraints
      = getDifferentiableConstraints();

  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(original.size(), innerDim);

  const double EPS = 1e-5;

  for (std::size_t i = 0; i < innerDim; i++)
  {
    Eigen::VectorXd perturbed = before;
    double posEps = EPS;

    Eigen::VectorXd newPlus;
    Eigen::VectorXd newMinus;

    while (true)
    {
      perturbed = before;
      perturbed(i) += posEps;
      setWrt(world, wrt, perturbed);

      BackpropSnapshotPtr plusBackptr = neural::forwardPass(world, true);
      Eigen::MatrixXd newP_c
          = plusBackptr->getProjectionIntoClampsMatrix(world);
      if (newP_c.rows() == originalP_c.rows())
      {
        newPlus = newP_c * v;
        break;
      }
      posEps *= 0.5;
    }

    perturbed = before;
    double negEps = EPS;
    while (true)
    {
      perturbed = before;
      perturbed(i) -= negEps;
      setWrt(world, wrt, perturbed);

      BackpropSnapshotPtr negBackptr = neural::forwardPass(world, true);

      Eigen::MatrixXd newP_c = getProjectionIntoClampsMatrix(world, true);
      if (newP_c.rows() == originalP_c.rows())
      {
        newMinus = newP_c * v;
        break;
      }
      negEps *= 0.5;
    }

    Eigen::VectorXd diff = newPlus - newMinus;
    result.col(i) = diff / (posEps + negEps);
  }

  setWrt(world, wrt, before);

  return result;
}

//==============================================================================
/// This computes and returns the jacobian of M^{-1}(pos, inertia) * tau by
/// finite differences.
Eigen::MatrixXd BackpropSnapshot::finiteDifferenceJacobianOfMinv(
    simulation::WorldPtr world, Eigen::VectorXd tau, WithRespectTo wrt)
{
  std::size_t innerDim = getWrtDim(world, wrt);

  // These are predicted contact forces at the clamping contacts
  Eigen::VectorXd original = implicitMultiplyByInvMassMatrix(world, tau);

  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(original.size(), innerDim);

  Eigen::VectorXd before = getWrt(world, wrt);

  const double EPS = 1e-7;

  for (std::size_t i = 0; i < innerDim; i++)
  {
    Eigen::VectorXd perturbed = before;
    perturbed(i) += EPS;
    setWrt(world, wrt, perturbed);
    Eigen::MatrixXd newVPlus = implicitMultiplyByInvMassMatrix(world, tau);
    perturbed = before;
    perturbed(i) -= EPS;
    setWrt(world, wrt, perturbed);
    Eigen::MatrixXd newVMinus = implicitMultiplyByInvMassMatrix(world, tau);
    Eigen::VectorXd diff = newVPlus - newVMinus;
    result.col(i) = diff / (2 * EPS);
  }

  setWrt(world, wrt, before);

  return result;
}

//==============================================================================
/// This computes and returns the jacobian of C(pos, inertia, vel) by finite
/// differences.
Eigen::MatrixXd BackpropSnapshot::finiteDifferenceJacobianOfC(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  std::size_t innerDim = getWrtDim(world, wrt);

  // These are predicted contact forces at the clamping contacts
  Eigen::VectorXd original = world->getCoriolisAndGravityAndExternalForces();

  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(original.size(), innerDim);

  Eigen::VectorXd before = getWrt(world, wrt);

  const double EPS = 1e-7;

  for (std::size_t i = 0; i < innerDim; i++)
  {
    Eigen::VectorXd perturbed = before;
    perturbed(i) += EPS;
    setWrt(world, wrt, perturbed);
    Eigen::MatrixXd tauPos = world->getCoriolisAndGravityAndExternalForces();
    perturbed = before;
    perturbed(i) -= EPS;
    setWrt(world, wrt, perturbed);
    Eigen::MatrixXd tauNeg = world->getCoriolisAndGravityAndExternalForces();
    Eigen::VectorXd diff = tauPos - tauNeg;
    result.col(i) = diff / (2 * EPS);
  }

  setWrt(world, wrt, before);

  return result;
}

//==============================================================================
/// This computes and returns the jacobian of M^{-1}(pos, inertia) * C(pos,
/// inertia, vel) by finite differences. This is SUPER SLOW, and is only here
/// for testing.
Eigen::MatrixXd BackpropSnapshot::finiteDifferenceJacobianOfMinvC(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  std::size_t innerDim = getWrtDim(world, wrt);

  // These are predicted contact forces at the clamping contacts
  Eigen::VectorXd original = implicitMultiplyByInvMassMatrix(
      world, mPreStepTorques - world->getCoriolisAndGravityAndExternalForces());

  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(original.size(), innerDim);

  Eigen::VectorXd before = getWrt(world, wrt);

  const double EPS = 1e-7;

  for (std::size_t i = 0; i < innerDim; i++)
  {
    Eigen::VectorXd perturbed = before;
    perturbed(i) += EPS;
    setWrt(world, wrt, perturbed);
    Eigen::MatrixXd tauPos = implicitMultiplyByInvMassMatrix(
        world,
        mPreStepTorques - world->getCoriolisAndGravityAndExternalForces());
    perturbed = before;
    perturbed(i) -= EPS;
    setWrt(world, wrt, perturbed);
    Eigen::MatrixXd tauNeg = implicitMultiplyByInvMassMatrix(
        world,
        mPreStepTorques - world->getCoriolisAndGravityAndExternalForces());
    Eigen::VectorXd diff = tauPos - tauNeg;
    result.col(i) = diff / (2 * EPS);
  }

  setWrt(world, wrt, before);

  return result;
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::finiteDifferenceJacobianOfConstraintForce(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  bool oldPenetrationCorrection
      = world->getConstraintSolver()->getPenetrationCorrectionEnabled();
  world->getConstraintSolver()->setPenetrationCorrectionEnabled(false);

  Eigen::VectorXd f0
      = neural::forwardPass(world, true)->getClampingConstraintImpulses();

  std::size_t innerDim = getWrtDim(world, wrt);

  Eigen::VectorXd before = getWrt(world, wrt);

  Eigen::MatrixXd result = Eigen::MatrixXd::Zero(f0.size(), innerDim);

  const double EPS = 1e-7;

  for (std::size_t i = 0; i < innerDim; i++)
  {
    Eigen::VectorXd fPlus;
    Eigen::VectorXd fMinus;

    Eigen::VectorXd perturbed = before;
    double epsPos = EPS;
    while (true)
    {
      perturbed = before;
      perturbed(i) += epsPos;
      setWrt(world, wrt, perturbed);
      BackpropSnapshotPtr perturbedPtr = neural::forwardPass(world, true);
      if (perturbedPtr->getNumClamping() == f0.size())
      {
        fPlus = perturbedPtr->getClampingConstraintImpulses();
        break;
      }
      epsPos *= 0.5;
    }

    double epsNeg = EPS;
    while (true)
    {
      perturbed = before;
      perturbed(i) -= epsNeg;
      setWrt(world, wrt, perturbed);
      BackpropSnapshotPtr perturbedPtr = neural::forwardPass(world, true);
      if (perturbedPtr->getNumClamping() == f0.size())
      {
        fMinus = perturbedPtr->getClampingConstraintImpulses();
        break;
      }
      epsNeg *= 0.5;
    }
    Eigen::VectorXd diff = fPlus - fMinus;
    result.col(i) = diff / (epsPos + epsNeg);
  }

  setWrt(world, wrt, before);
  world->getConstraintSolver()->setPenetrationCorrectionEnabled(
      oldPenetrationCorrection);

  return result;
}

//==============================================================================
std::size_t BackpropSnapshot::getWrtDim(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  if (wrt == WithRespectTo::POSITION || wrt == WithRespectTo::VELOCITY
      || wrt == WithRespectTo::FORCE)
  {
    return world->getNumDofs();
  }
  else if (wrt == WithRespectTo::LINK_MASSES)
  {
    return world->getLinkMassesDims();
  }
  else if (wrt == WithRespectTo::LINK_COMS)
  {
    return world->getLinkCOMDims();
  }
  else if (wrt == WithRespectTo::LINK_MOIS)
  {
    return world->getLinkMOIDims();
  }
  else
  {
    assert(false && "Unrecognized wrt passed to getWrtDim()");
    return 0;
  }
}

//==============================================================================
Eigen::VectorXd BackpropSnapshot::getWrt(
    simulation::WorldPtr world, WithRespectTo wrt)
{
  if (wrt == POSITION)
  {
    return world->getPositions();
  }
  else if (wrt == VELOCITY)
  {
    return world->getVelocities();
  }
  else if (wrt == FORCE)
  {
    return world->getForces();
  }
  else if (wrt == WithRespectTo::LINK_MASSES)
  {
    return world->getLinkMasses();
  }
  else if (wrt == WithRespectTo::LINK_COMS)
  {
    return world->getLinkCOMs();
  }
  else if (wrt == WithRespectTo::LINK_MOIS)
  {
    return world->getLinkMOIs();
  }
  else
  {
    assert(false && "Unrecognized wrt passed to getWrt()");
    return Eigen::VectorXd::Zero(0);
  }
}

//==============================================================================
void BackpropSnapshot::setWrt(
    simulation::WorldPtr world, WithRespectTo wrt, Eigen::VectorXd v)
{
  if (wrt == POSITION)
  {
    world->setPositions(v);
  }
  else if (wrt == VELOCITY)
  {
    world->setVelocities(v);
  }
  else if (wrt == FORCE)
  {
    world->setForces(v);
  }
  else if (wrt == WithRespectTo::LINK_MASSES)
  {
    world->setLinkMasses(v);
  }
  else if (wrt == WithRespectTo::LINK_COMS)
  {
    world->setLinkCOMs(v);
  }
  else if (wrt == WithRespectTo::LINK_MOIS)
  {
    world->setLinkMOIs(v);
  }
  else
  {
    assert(false && "Unrecognized wrt passed to setWrt()");
  }
}

//==============================================================================
Eigen::MatrixXd BackpropSnapshot::assembleMatrix(
    WorldPtr world, MatrixToAssemble whichMatrix)
{
  std::size_t numCols = 0;
  if (whichMatrix == MatrixToAssemble::CLAMPING
      || whichMatrix == MatrixToAssemble::MASSED_CLAMPING)
    numCols = mNumClamping;
  else if (
      whichMatrix == MatrixToAssemble::UPPER_BOUND
      || whichMatrix == MatrixToAssemble::MASSED_UPPER_BOUND)
    numCols = mNumUpperBound;
  else if (whichMatrix == MatrixToAssemble::BOUNCING)
    numCols = mNumBouncing;

  Eigen::MatrixXd matrix = Eigen::MatrixXd(mNumDOFs, numCols);
  matrix.setZero();
  std::size_t constraintCursor = 0;
  for (std::size_t i = 0; i < mGradientMatrices.size(); i++)
  {
    Eigen::MatrixXd groupMatrix;

    if (whichMatrix == MatrixToAssemble::CLAMPING)
      groupMatrix = mGradientMatrices[i]->getClampingConstraintMatrix();
    else if (whichMatrix == MatrixToAssemble::MASSED_CLAMPING)
      groupMatrix = mGradientMatrices[i]->getMassedClampingConstraintMatrix();
    else if (whichMatrix == MatrixToAssemble::UPPER_BOUND)
      groupMatrix = mGradientMatrices[i]->getUpperBoundConstraintMatrix();
    else if (whichMatrix == MatrixToAssemble::MASSED_UPPER_BOUND)
      groupMatrix = mGradientMatrices[i]->getMassedUpperBoundConstraintMatrix();
    else if (whichMatrix == MatrixToAssemble::BOUNCING)
      groupMatrix = mGradientMatrices[i]->getBouncingConstraintMatrix();

    // shuffle the clamps into the main matrix
    std::size_t dofCursorGroup = 0;
    for (std::size_t k = 0; k < mGradientMatrices[i]->getSkeletons().size();
         k++)
    {
      SkeletonPtr skel
          = world->getSkeleton(mGradientMatrices[i]->getSkeletons()[k]);
      // This maps to the row in the world matrix
      std::size_t dofCursorWorld = mSkeletonOffset[skel->getName()];

      // The source block in the groupClamps matrix is a row section at
      // (dofCursorGroup, 0) of full width (skel->getNumDOFs(),
      // groupClamps.cols()), which we want to copy into our unified
      // clampingConstraintMatrix.

      // The destination block in clampingConstraintMatrix is the column
      // corresponding to this constraint group's constraint set, and the row
      // corresponding to this skeleton's offset into the world at
      // (dofCursorWorld, constraintCursor).

      matrix.block(
          dofCursorWorld,
          constraintCursor,
          skel->getNumDofs(),
          groupMatrix.cols())
          = groupMatrix.block(
              dofCursorGroup, 0, skel->getNumDofs(), groupMatrix.cols());

      dofCursorGroup += skel->getNumDofs();
    }

    constraintCursor += groupMatrix.cols();
  }
  return matrix;
}

Eigen::MatrixXd BackpropSnapshot::assembleBlockDiagonalMatrix(
    simulation::WorldPtr world,
    BackpropSnapshot::BlockDiagonalMatrixToAssemble whichMatrix,
    bool forFiniteDifferencing)
{
  Eigen::MatrixXd J = Eigen::MatrixXd(mNumDOFs, mNumDOFs);
  J.setZero();

  // If we're not finite differencing, then set the state of the world back to
  // what it was during the forward pass, so that implicit mass matrix
  // computations work correctly.

  Eigen::VectorXd oldPositions = world->getPositions();
  Eigen::VectorXd oldVelocities = world->getVelocities();
  if (!forFiniteDifferencing)
  {
    world->setPositions(mPreStepPosition);
    world->setVelocities(mPreStepVelocity);
  }

  std::size_t cursor = 0;
  for (std::size_t i = 0; i < world->getNumSkeletons(); i++)
  {
    std::size_t skelDOF = world->getSkeleton(i)->getNumDofs();
    if (whichMatrix == BackpropSnapshot::BlockDiagonalMatrixToAssemble::MASS)
    {
      J.block(cursor, cursor, skelDOF, skelDOF)
          = world->getSkeleton(i)->getMassMatrix();
    }
    else if (
        whichMatrix
        == BackpropSnapshot::BlockDiagonalMatrixToAssemble::INV_MASS)
    {
      J.block(cursor, cursor, skelDOF, skelDOF)
          = world->getSkeleton(i)->getInvMassMatrix();
    }
    else if (
        whichMatrix == BackpropSnapshot::BlockDiagonalMatrixToAssemble::POS_C)
    {
      J.block(cursor, cursor, skelDOF, skelDOF)
          = world->getSkeleton(i)->getJacobianOfC(WithRespectTo::POSITION);
    }
    else if (
        whichMatrix == BackpropSnapshot::BlockDiagonalMatrixToAssemble::VEL_C)
    {
      J.block(cursor, cursor, skelDOF, skelDOF)
          = world->getSkeleton(i)->getVelCJacobian();
    }
    cursor += skelDOF;
  }

  // If we're not finite differencing, reset the position of the world to what
  // it was before

  if (!forFiniteDifferencing)
  {
    world->setPositions(oldPositions);
    world->setVelocities(oldVelocities);
  }

  return J;
}

//==============================================================================
template <typename Vec>
Vec BackpropSnapshot::assembleVector(VectorToAssemble whichVector)
{
  // When we're assembling vectors related to contact constraints, we can put
  // them in order of constraint groups
  if (whichVector == BOUNCE_DIAGONALS || whichVector == RESTITUTION_DIAGONALS
      || whichVector == CONTACT_CONSTRAINT_IMPULSES
      || whichVector == CONTACT_CONSTRAINT_MAPPINGS
      || whichVector == PENETRATION_VELOCITY_HACK
      || whichVector == CLAMPING_CONSTRAINT_IMPULSES
      || whichVector == CLAMPING_CONSTRAINT_RELATIVE_VELS)
  {
    if (mGradientMatrices.size() == 1)
    {
      return getVectorToAssemble<Vec>(mGradientMatrices[0], whichVector);
    }

    std::size_t size = 0;
    for (std::size_t i = 0; i < mGradientMatrices.size(); i++)
    {
      // BOUNCE_DIAGONALS: bounce size is number of clamping contacts for each
      // group RESTITUTION_DIAGONALS: bounce size is number of bouncing contacts
      // (which is usually less than the number of clamping contacts) for each
      // group CONTACT_CONSTRAINT_IMPULSES: This is the total number of
      // contacts, including non-clamping ones CONTACT_CONSTRAINT_MAPPINGS: This
      // is the total number of contacts, including non-clamping ones
      size
          += getVectorToAssemble<Vec>(mGradientMatrices[i], whichVector).size();
    }

    Vec collected = Vec(size);

    std::size_t cursor = 0;
    for (std::size_t i = 0; i < mGradientMatrices.size(); i++)
    {
      const Vec& vec
          = getVectorToAssemble<Vec>(mGradientMatrices[i], whichVector);
      collected.segment(cursor, vec.size()) = vec;
      cursor += vec.size();
    }
    return collected;
  }
  // The other types of vectors need to go in order of skeletons
  else
  {
    Vec collected = Vec(mNumDOFs);
    collected.setZero();

    for (std::size_t i = 0; i < mGradientMatrices.size(); i++)
    {
      const Vec& vec
          = getVectorToAssemble<Vec>(mGradientMatrices[i], whichVector);
      int groupCursor = 0;
      for (auto skelName : mGradientMatrices[i]->getSkeletons())
      {
        int dofs = mSkeletonDofs[skelName];
        int worldOffset = mSkeletonOffset[skelName];
        collected.segment(worldOffset, dofs) = vec.segment(groupCursor, dofs);
        groupCursor += dofs;
      }
    }
    return collected;
  }
}

//==============================================================================
template <>
const Eigen::VectorXd& BackpropSnapshot::getVectorToAssemble(
    std::shared_ptr<ConstrainedGroupGradientMatrices> matrices,
    VectorToAssemble whichVector)
{
  if (whichVector == VectorToAssemble::BOUNCE_DIAGONALS)
    return matrices->getBounceDiagonals();
  if (whichVector == VectorToAssemble::RESTITUTION_DIAGONALS)
    return matrices->getRestitutionDiagonals();
  if (whichVector == VectorToAssemble::CONTACT_CONSTRAINT_IMPULSES)
    return matrices->getContactConstraintImpluses();
  if (whichVector == VectorToAssemble::PENETRATION_VELOCITY_HACK)
    return matrices->getPenetrationCorrectionVelocities();
  if (whichVector == VectorToAssemble::CLAMPING_CONSTRAINT_IMPULSES)
    return matrices->getClampingConstraintImpulses();
  if (whichVector == VectorToAssemble::CLAMPING_CONSTRAINT_RELATIVE_VELS)
    return matrices->getClampingConstraintRelativeVels();
  if (whichVector == VectorToAssemble::VEL_DUE_TO_ILLEGAL)
    return matrices->getVelocityDueToIllegalImpulses();
  if (whichVector == VectorToAssemble::PRE_STEP_VEL)
    return matrices->getPreStepVelocity();
  if (whichVector == VectorToAssemble::PRE_STEP_TAU)
    return matrices->getPreStepTorques();
  if (whichVector == VectorToAssemble::PRE_LCP_VEL)
    return matrices->getPreLCPVelocity();
  if (whichVector == VectorToAssemble::CORIOLIS_AND_GRAVITY)
    return matrices->getCoriolisAndGravityForces();

  assert(whichVector != VectorToAssemble::CONTACT_CONSTRAINT_MAPPINGS);
  // Control will never reach this point, but this removes a warning
  throw 1;
}

template <>
const Eigen::VectorXi& BackpropSnapshot::getVectorToAssemble(
    std::shared_ptr<ConstrainedGroupGradientMatrices> matrices,
    VectorToAssemble whichVector)
{
  assert(whichVector == VectorToAssemble::CONTACT_CONSTRAINT_MAPPINGS);
  return matrices->getContactConstraintMappings();
}

} // namespace neural
} // namespace dart