#include "dart/biomechanics/IKInitializer.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <Eigen/Eigenvalues>
#include <Eigen/SVD>

#include "dart/dynamics/BallJoint.hpp"
#include "dart/dynamics/BodyNode.hpp"
#include "dart/dynamics/EulerFreeJoint.hpp"
#include "dart/dynamics/EulerJoint.hpp"
#include "dart/dynamics/FreeJoint.hpp"
#include "dart/dynamics/Joint.hpp"
#include "dart/dynamics/RevoluteJoint.hpp"
#include "dart/dynamics/UniversalJoint.hpp"
#include "dart/math/MathTypes.hpp"

namespace dart {
namespace biomechanics {

//==============================================================================
/// This is a helper struct that is used to simplify the code in
/// estimatePosesClosedForm()
typedef struct AnnotatedStackedBody
{
  std::shared_ptr<struct StackedBody> stackedBody;
  std::vector<Eigen::Isometry3s> relativeTransforms;
  std::vector<std::shared_ptr<struct StackedJoint>> adjacentJoints;
  std::vector<Eigen::Vector3s> adjacentJointCenters;
  std::vector<std::string> adjacentMarkers;
  std::vector<Eigen::Vector3s> adjacentMarkerCenters;
} AnnotatedStackedBody;

//==============================================================================
Eigen::Vector3s getStackedJointCenterFromJointCentersVector(
    std::shared_ptr<struct StackedJoint> joint,
    Eigen::VectorXs jointWorldCenters)
{
  Eigen::Vector3s jointWorldCenter = Eigen::Vector3s::Zero();
  for (dynamics::Joint* joint : joint->joints)
  {
    jointWorldCenter
        += jointWorldCenters.segment<3>(joint->getJointIndexInSkeleton() * 3);
  }
  jointWorldCenter /= joint->joints.size();
  return jointWorldCenter;
}

//==============================================================================
/// This returns true if the given body is the parent of the joint OR if
/// there's a hierarchy of fixed joints that connect it to the parent
bool isDynamicParentOfJoint(std::string bodyName, dynamics::Joint* joint)
{
  while (true)
  {
    if (joint->getParentBodyNode() == nullptr)
      return false;
    if (bodyName == joint->getParentBodyNode()->getName())
    {
      return true;
    }
    // Recurse up the chain, as long as we're traversing only fixed joints
    if (joint->getParentBodyNode()->getParentJoint() != nullptr
        && joint->getParentBodyNode()->getParentJoint()->isFixed())
    {
      joint = joint->getParentBodyNode()->getParentJoint();
    }
    else
    {
      return false;
    }
  }
}

//==============================================================================
/// This returns true if the given body is the child of the joint OR if
/// there's a hierarchy of fixed joints that connect it to the child
bool isDynamicChildOfJoint(std::string bodyName, dynamics::Joint* joint)
{
  while (true)
  {
    if (joint->getChildBodyNode() == nullptr)
      return false;
    if (bodyName == joint->getChildBodyNode()->getName())
    {
      return true;
    }
    // Recurse down the chain, as long as we're traversing only fixed joints
    if (joint->getChildBodyNode()->getNumChildJoints() == 1
        && joint->getChildBodyNode()->getChildJoint(0)->isFixed())
    {
      joint = joint->getChildBodyNode()->getChildJoint(0);
    }
    else
    {
      return false;
    }
  }
}

//==============================================================================
bool isCoplanar(const std::vector<Eigen::Vector3s>& points)
{
  // Ensure there are at least 4 points
  if (points.size() < 4)
  {
    return true;
  }

  // Choose the first three points and form two vectors
  const Eigen::Vector3s& p1 = points[0];
  const Eigen::Vector3s& p2 = points[1];
  const Eigen::Vector3s& p3 = points[2];
  const Eigen::Vector3s v1 = p2 - p1;
  const Eigen::Vector3s v2 = p3 - p1;

  // Calculate the cross-product of the two vectors to get a normal vector
  const Eigen::Vector3s normal = v1.cross(v2).normalized();

  // Check if all remaining points are coplanar with the first three points
  for (size_t i = 3; i < points.size(); ++i)
  {
    // Form a vector from one of the original three points to the current point
    const Eigen::Vector3s& point = points[i];
    const Eigen::Vector3s v = point - p1;

    // Calculate the dot product of the vector with the normal vector
    const double dot_product = v.dot(normal);

    // Check if the dot product is greater zero, and if so we have found a
    // non-coplanar point
    const s_t threshold = 1e-3;
    if (std::abs(dot_product) > threshold)
    {
      return false;
    }
  }

  return true;
}

//==============================================================================
bool isPositiveDefinite(const Eigen::MatrixXd& A)
{
  if (A.rows() != A.cols())
    return false;

  // Check symmetry
  if (!A.isApprox(A.transpose()))
    return false;

  // Compute eigenvalues
  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(A);

  // Check for success
  if (solver.info() != Eigen::Success)
    return false;

  // Check if all eigenvalues are positive
  if ((solver.eigenvalues().array() > 0).all())
    return true;
  else
  {
    std::cout << "Got non-positive eigenvalues: " << solver.eigenvalues()
              << std::endl;
    return false;
  }
}

//==============================================================================
Eigen::Vector3s ensureOnSameSideOfPlane(
    const std::vector<Eigen::Vector3s>& neutralPoints,
    Eigen::Vector3s neutralGoal,
    std::vector<Eigen::Vector3s> actualPoints,
    Eigen::Vector3s ambiguousReconstructionFromActualPoints)
{
  if (neutralPoints.size() < 3 || actualPoints.size() < 3)
  {
    return ambiguousReconstructionFromActualPoints;
  }

  // Choose the first three points and form two vectors
  const Eigen::Vector3s neutralNormal
      = ((neutralPoints[1] - neutralPoints[0])
             .cross(neutralPoints[2] - neutralPoints[0]))
            .normalized();
  s_t neutralGoalDistanceFromNormal
      = (neutralGoal - neutralPoints[0]).dot(neutralNormal);

  const Eigen::Vector3s actualNormal
      = ((actualPoints[1] - actualPoints[0])
             .cross(actualPoints[2] - actualPoints[0]))
            .normalized();
  s_t ambiguousReconstructionDistanceFromNormal
      = (ambiguousReconstructionFromActualPoints - actualPoints[0])
            .dot(actualNormal);

  if (neutralGoalDistanceFromNormal * ambiguousReconstructionDistanceFromNormal
      < 0)
  {
    // If they're on opposite sides of the plane, flip the `actualGoal` to be on
    // the same side of the plane as the `neutralGoal`.
    return ambiguousReconstructionFromActualPoints
           - 2 * ambiguousReconstructionDistanceFromNormal * actualNormal;
  }
  else
  {
    // If they're already on the same side of the plane, don't worry
    return ambiguousReconstructionFromActualPoints;
  }
}

//==============================================================================
IKInitializer::IKInitializer(
    std::shared_ptr<dynamics::Skeleton> skel,
    std::map<std::string, std::pair<dynamics::BodyNode*, Eigen::Vector3s>>
        markers,
    std::vector<std::map<std::string, Eigen::Vector3s>> markerObservations,
    s_t modelHeightM)
  : mSkel(skel),
    mMarkerObservations(markerObservations),
    mModelHeightM(modelHeightM)
{
  // 1. Convert the marker map to an ordered list
  for (auto& pair : markers)
  {
    mMarkerNames.push_back(pair.first);
    mMarkers.push_back(pair.second);
  }

  Eigen::VectorXs oldPositions = mSkel->getPositions();
  mSkel->setPositions(Eigen::VectorXs::Zero(mSkel->getNumDofs()));
  Eigen::VectorXs oldScales = mSkel->getBodyScales();
  if (modelHeightM > 0)
  {
    mSkel->setBodyScales(Eigen::VectorXs::Ones(mSkel->getNumBodyNodes() * 3));
    s_t defaultHeight
        = mSkel->getHeight(Eigen::VectorXs::Zero(mSkel->getNumDofs()));
    if (defaultHeight > 0)
    {
      s_t ratio = modelHeightM / defaultHeight;
      mSkel->setBodyScales(mSkel->getBodyScales() * ratio);
      s_t newHeight
          = mSkel->getHeight(Eigen::VectorXs::Zero(mSkel->getNumDofs()));
      (void)newHeight;
      assert(abs(newHeight - modelHeightM) < 1e-6);
    }
  }

  // 2. Create the simplified skeleton
  Eigen::VectorXs jointWorldPositions
      = mSkel->getJointWorldPositions(mSkel->getJoints());

  // 2.1. Set up the initial stacked bodies and joints, with appropriate
  // pointers to each other
  mStackedJoints.clear();
  mStackedBodies.clear();
  for (int i = 0; i < mSkel->getNumBodyNodes(); i++)
  {
    mStackedBodies.push_back(std::make_shared<struct StackedBody>());
    mStackedBodies[i]->bodies.push_back(mSkel->getBodyNode(i));
    mStackedBodies[i]->name = mSkel->getBodyNode(i)->getName();
  }
  for (int i = 0; i < mSkel->getNumJoints(); i++)
  {
    mStackedJoints.push_back(std::make_shared<struct StackedJoint>());
    mStackedJoints[i]->joints.push_back(mSkel->getJoint(i));
    mStackedJoints[i]->name = mSkel->getJoint(i)->getName();
  }
  for (int i = 0; i < mSkel->getNumBodyNodes(); i++)
  {
    dynamics::BodyNode* body = mSkel->getBodyNode(i);
    assert(body->getParentJoint() != nullptr);
    mStackedBodies[i]->parentJoint
        = mStackedJoints[body->getParentJoint()->getJointIndexInSkeleton()];
    for (int j = 0; j < body->getNumChildJoints(); j++)
    {
      mStackedBodies[i]->childJoints.push_back(
          mStackedJoints[body->getChildJoint(j)->getJointIndexInSkeleton()]);
    }
  }
  for (int i = 0; i < mSkel->getNumJoints(); i++)
  {
    dynamics::Joint* joint = mSkel->getJoint(i);
    assert(joint->getChildBodyNode() != nullptr);
    mStackedJoints[i]->childBody
        = mStackedBodies[joint->getChildBodyNode()->getIndexInSkeleton()];
    if (joint->getParentBodyNode() == nullptr)
    {
      mStackedJoints[i]->parentBody = nullptr;
    }
    else
    {
      mStackedJoints[i]->parentBody
          = mStackedBodies[joint->getParentBodyNode()->getIndexInSkeleton()];
    }
  }
  // At this stage, we should have exactly the same number of "stacked" nodes as
  // original nodes, since no stacking has yet taken place.
  assert(mStackedBodies.size() == mSkel->getNumBodyNodes());
  assert(mStackedJoints.size() == mSkel->getNumJoints());

  // 2.2. Collapse the stacked bodies and joints where there are joints that are
  // parent/child and are "too close" in physical space, which indicates that
  // the skeleton architect is using multiple low-DOF joints to represent a
  // single more complex high-DOF joint.

  while (true)
  {
    bool anyToMerge = false;
    std::pair<
        std::shared_ptr<struct StackedJoint>,
        std::shared_ptr<struct StackedJoint>>
        toMerge;

    for (int i = 0; i < mStackedJoints.size(); i++)
    {
      Eigen::Vector3s joint1WorldCenter
          = getStackedJointCenterFromJointCentersVector(
              mStackedJoints[i], jointWorldPositions);
      for (auto& childJoint : mStackedJoints[i]->childBody->childJoints)
      {
        Eigen::Vector3s joint2WorldCenter
            = getStackedJointCenterFromJointCentersVector(
                childJoint, jointWorldPositions);
        Eigen::Vector3s joint1ToJoint2 = joint2WorldCenter - joint1WorldCenter;
        s_t joint1ToJoint2Distance = joint1ToJoint2.norm();

        if (joint1ToJoint2Distance < 0.07)
        {
          anyToMerge = true;
          toMerge = std::make_pair(mStackedJoints[i], childJoint);
          break;
        }
      }
      if (anyToMerge)
        break;
    }

    if (anyToMerge)
    {
      // 2.2.1. Merge the joints
      std::shared_ptr<struct StackedJoint> mergedJoint = toMerge.first;
      std::shared_ptr<struct StackedJoint> jointToDelete = toMerge.second;
      std::shared_ptr<struct StackedBody> bodyToDelete
          = jointToDelete->parentBody;
      std::shared_ptr<struct StackedBody> mergedBody = jointToDelete->childBody;
      std::cout << "Merging joints \"" << jointToDelete->name << "\" into \""
                << mergedJoint->name << "\"" << std::endl;
      std::cout << " -> As a result merging bodies \"" << bodyToDelete->name
                << "\" into \"" << mergedBody->name << "\"" << std::endl;

      mergedJoint->joints.insert(
          mergedJoint->joints.end(),
          jointToDelete->joints.begin(),
          jointToDelete->joints.end());
      mergedBody->bodies.insert(
          mergedBody->bodies.end(),
          bodyToDelete->bodies.begin(),
          bodyToDelete->bodies.end());
      mergedJoint->childBody = mergedBody;
      mergedBody->parentJoint = mergedJoint;

      // 2.2.2. Remove the joint and body from the lists
      auto jointToDeleteIterator = std::find(
          mStackedJoints.begin(), mStackedJoints.end(), jointToDelete);
      assert(jointToDeleteIterator != mStackedJoints.end());
      mStackedJoints.erase(jointToDeleteIterator);
      auto bodyToDeleteIterator = std::find(
          mStackedBodies.begin(), mStackedBodies.end(), bodyToDelete);
      assert(bodyToDeleteIterator != mStackedBodies.end());
      mStackedBodies.erase(bodyToDeleteIterator);
    }
    else
    {
      break;
    }
  }

  // 2.3. Collapse the stacked bodies that are connected by a fixed joint, and
  // simply remove the fixed joint.

  while (true)
  {
    std::shared_ptr<struct StackedJoint> toCollapse = nullptr;

    for (int i = 0; i < mStackedJoints.size(); i++)
    {
      bool allFixed = true;
      for (auto* joint : mStackedJoints[i]->joints)
      {
        if (!joint->isFixed())
        {
          allFixed = false;
          break;
        }
      }
      if (allFixed)
      {
        toCollapse = mStackedJoints[i];
      }
    }

    if (toCollapse != nullptr)
    {
      // 2.3.1. Merge the connected bodies
      std::shared_ptr<struct StackedBody> parentBody = toCollapse->parentBody;
      std::shared_ptr<struct StackedBody> childBody = toCollapse->childBody;
      std::cout << "Discovered locked joint \"" << toCollapse->name << "\""
                << std::endl;
      std::cout << " -> As a result merging bodies \"" << childBody->name
                << "\" into \"" << parentBody->name << "\"" << std::endl;
      parentBody->bodies.insert(
          parentBody->bodies.end(),
          childBody->bodies.begin(),
          childBody->bodies.end());
      // Re-attach all the child joints to the parent body
      for (auto& childJoint : childBody->childJoints)
      {
        childJoint->parentBody = parentBody;
        parentBody->childJoints.push_back(childJoint);
      }

      // 2.3.2. Remove the joint and body from the lists
      auto jointToDeleteIterator
          = std::find(mStackedJoints.begin(), mStackedJoints.end(), toCollapse);
      assert(jointToDeleteIterator != mStackedJoints.end());
      mStackedJoints.erase(jointToDeleteIterator);
      auto bodyToDeleteIterator
          = std::find(mStackedBodies.begin(), mStackedBodies.end(), childBody);
      assert(bodyToDeleteIterator != mStackedBodies.end());
      mStackedBodies.erase(bodyToDeleteIterator);
      auto childJointToDeletIterator = std::find(
          parentBody->childJoints.begin(),
          parentBody->childJoints.end(),
          toCollapse);
      assert(jointToDeleteIterator != parentBody->childJoints.end());
      parentBody->childJoints.erase(childJointToDeletIterator);
    }
    else
    {
      break;
    }
  }

  // 3. Filter to just the stacked joints that are connected to at least three
  // markers, since none of our closed-form algorithms will work with less than
  // that, and the iterative algorithms don't use the stacked joints. While
  // we're at it, measure the distances between each joint center and the
  // adjacent markers.
  Eigen::VectorXs markerWorldPositions
      = mSkel->getMarkerWorldPositions(mMarkers);
  std::vector<std::shared_ptr<struct StackedJoint>> stackedJointsToRemove;
  for (int i = 0; i < mStackedJoints.size(); i++)
  {
    Eigen::Vector3s jointWorldCenter
        = getStackedJointCenterFromJointCentersVector(
            mStackedJoints[i], jointWorldPositions);

    std::map<std::string, s_t> jointToMarkerSquaredDistances;

    for (int j = 0; j < mMarkers.size(); j++)
    {
      assert(mStackedJoints[i]->childBody != nullptr);
      if (mStackedJoints[i]->parentBody != nullptr
          && std::find(
                 mStackedJoints[i]->parentBody->bodies.begin(),
                 mStackedJoints[i]->parentBody->bodies.end(),
                 mMarkers[j].first)
                 != mStackedJoints[i]->parentBody->bodies.end())
      {
        jointToMarkerSquaredDistances[mMarkerNames[j]]
            = (jointWorldCenter - markerWorldPositions.segment<3>(j * 3))
                  .squaredNorm();
      }
      else if (
          std::find(
              mStackedJoints[i]->childBody->bodies.begin(),
              mStackedJoints[i]->childBody->bodies.end(),
              mMarkers[j].first)
          != mStackedJoints[i]->childBody->bodies.end())
      {
        jointToMarkerSquaredDistances[mMarkerNames[j]]
            = (jointWorldCenter - markerWorldPositions.segment<3>(j * 3))
                  .squaredNorm();
      }
    }

    if (jointToMarkerSquaredDistances.size() >= 3)
    {
      mJointToMarkerSquaredDistances[mStackedJoints[i]->name]
          = jointToMarkerSquaredDistances;
    }
    else
    {
      stackedJointsToRemove.push_back(mStackedJoints[i]);
    }
  }
  for (int i = 0; i < stackedJointsToRemove.size(); i++)
  {
    mStackedJoints.erase(
        std::find(
            mStackedJoints.begin(),
            mStackedJoints.end(),
            stackedJointsToRemove[i]),
        mStackedJoints.end());
  }

  // 4. See if there are any connected pairs of joints, and if so measure the
  // distance between them on the skeleton.
  for (int i = 0; i < mStackedJoints.size(); i++)
  {
    std::shared_ptr<struct StackedJoint> joint1 = mStackedJoints[i];
    Eigen::Vector3s joint1WorldCenter
        = getStackedJointCenterFromJointCentersVector(
            joint1, jointWorldPositions);
    for (int j = i + 1; j < mStackedJoints.size(); j++)
    {
      std::shared_ptr<struct StackedJoint> joint2 = mStackedJoints[j];
      Eigen::Vector3s joint2WorldCenter
          = getStackedJointCenterFromJointCentersVector(
              joint2, jointWorldPositions);

      // 3.1. If the joints are connected to the same body, then record them
      if (joint1->parentBody == joint2->childBody
          || joint2->parentBody == joint1->childBody
          || joint1->parentBody == joint2->parentBody)
      {
        Eigen::Vector3s joint1ToJoint2 = joint2WorldCenter - joint1WorldCenter;
        s_t joint1ToJoint2SquaredDistance = joint1ToJoint2.squaredNorm();
        assert(joint1ToJoint2SquaredDistance > 0);

        mJointToJointSquaredDistances[joint1->name][joint2->name]
            = joint1ToJoint2SquaredDistance;
        mJointToJointSquaredDistances[joint2->name][joint1->name]
            = joint1ToJoint2SquaredDistance;
      }
    }
  }

  mSkel->setPositions(oldPositions);
  mSkel->setBodyScales(oldScales);
}

//==============================================================================
/// This runs the full IK initialization algorithm, and leaves the answers in
/// the public fields of this class
void IKInitializer::runFullPipeline(bool logOutput)
{
  // Use MDS, despite its many flaws, to arrive at decent initial guesses for
  // joint centers that we can use to center the subsequent least-squares fits
  // if a joint doesn't move very much during a trial (like if your arms are at
  // your side during a whole trial).
  closedFormMDSJointCenterSolver();
  // Use the pivot finding, where there is the huge wealth of marker information
  // (3+ markers on adjacent body segments) to make it possible
  closedFormPivotFindingJointCenterSolver(logOutput);
  recenterAxisJointsBasedOnBoneAngles();
  // Fill in the parts of the body scales and poses that we can in closed form
  estimateGroupScalesClosedForm();
  estimatePosesClosedForm();
}

//==============================================================================
/// For each timestep, and then for each joint, this sets up and runs an MDS
/// algorithm to triangulate the joint center location from the known marker
/// locations of the markers attached to the joint's two body segments, and
/// then known distance from the joint center to each marker.
s_t IKInitializer::closedFormMDSJointCenterSolver(bool logOutput)
{
  // 0. Save the default pose and scale's version of the joint centers and
  // marker locations, so that we can use it to detect and resolve coplanar
  // ambiguity in subsequent steps.
  Eigen::VectorXs oldPositions = mSkel->getPositions();
  Eigen::VectorXs oldScales = mSkel->getBodyScales();
  mSkel->setPositions(Eigen::VectorXs::Zero(mSkel->getNumDofs()));
  mSkel->setBodyScales(Eigen::VectorXs::Ones(mSkel->getNumBodyNodes() * 3));
  Eigen::VectorXs neutralSkelJointWorldPositions
      = mSkel->getJointWorldPositions(mSkel->getJoints());
  Eigen::VectorXs neutralSkelMarkerWorldPositions
      = mSkel->getMarkerWorldPositions(mMarkers);
  mSkel->setPositions(oldPositions);
  mSkel->setBodyScales(oldScales);
  std::map<std::string, Eigen::Vector3s>
      neutralSkelJointCenterWorldPositionsMap;
  for (int i = 0; i < mStackedJoints.size(); i++)
  {
    neutralSkelJointCenterWorldPositionsMap[mStackedJoints[i]->name]
        = getStackedJointCenterFromJointCentersVector(
            mStackedJoints[i], neutralSkelJointWorldPositions);
  }
  std::map<std::string, Eigen::Vector3s> neutralSkelMarkerWorldPositionsMap;
  for (int i = 0; i < mMarkerNames.size(); i++)
  {
    neutralSkelMarkerWorldPositionsMap[mMarkerNames[i]]
        = neutralSkelMarkerWorldPositions.segment<3>(i * 3);
  }

  s_t totalMarkerError = 0.0;
  int count = 0;
  mJointCenters.clear();
  for (int t = 0; t < mMarkerObservations.size(); t++)
  {
    std::vector<std::shared_ptr<struct StackedJoint>> joints
        = getVisibleJoints(t);
    std::map<std::string, Eigen::Vector3s> lastSolvedJointCenters;
    std::map<std::string, Eigen::Vector3s> solvedJointCenters;
    while (true)
    {
      for (std::shared_ptr<struct StackedJoint> joint : joints)
      {
        // If we already solved this joint, no need to go again
        // if (lastSolvedJointCenters.count(joint->name))
        //   continue;

        std::vector<Eigen::Vector3s> adjacentPointLocations;
        std::vector<s_t> adjacentPointSquaredDistances;
        // These are the locations of each of the adjacent points, but on the
        // unscaled skeleton in the neutral pose, which we use to detect and
        // resolve coplanar ambiguity during reconstruction.
        std::vector<Eigen::Vector3s> adjacentPointLocationsInNeutralSkel;

        // 1. Find the markers that are adjacent to this joint and visible on
        // this frame
        for (auto& pair : mJointToMarkerSquaredDistances[joint->name])
        {
          if (mMarkerObservations[t].count(pair.first))
          {
            adjacentPointLocations.push_back(
                mMarkerObservations[t][pair.first]);
            adjacentPointSquaredDistances.push_back(pair.second);
            adjacentPointLocationsInNeutralSkel.push_back(
                neutralSkelMarkerWorldPositionsMap[pair.first]);
          }
        }

        for (auto& pair : mJointToJointSquaredDistances[joint->name])
        {
          if (lastSolvedJointCenters.count(pair.first))
          {
            adjacentPointLocations.push_back(
                lastSolvedJointCenters[pair.first]);
            adjacentPointSquaredDistances.push_back(pair.second);
            adjacentPointLocationsInNeutralSkel.push_back(
                neutralSkelJointCenterWorldPositionsMap[pair.first]);
          }
        }

        if (adjacentPointLocations.size() < 3)
          continue;

        // 2. Setup the squared-distance matrix
        int dim = adjacentPointLocations.size() + 1;
        Eigen::MatrixXs D = Eigen::MatrixXs::Zero(dim, dim);
        for (int i = 0; i < adjacentPointLocations.size(); i++)
        {
          for (int j = i + 1; j < adjacentPointLocations.size(); j++)
          {
            D(i, j) = (adjacentPointLocations[i] - adjacentPointLocations[j])
                          .squaredNorm();
            // The distance matrix is symmetric
            D(j, i) = D(i, j);
          }
        }
        for (int i = 0; i < adjacentPointLocations.size(); i++)
        {
          D(i, dim - 1) = adjacentPointSquaredDistances[i];
          D(dim - 1, i) = D(i, dim - 1);
        }

        // 3. Solve the distance matrix
        Eigen::MatrixXs pointCloud = getPointCloudFromDistanceMatrix(D);
        assert(!pointCloud.hasNaN());
        Eigen::MatrixXs transformed
            = mapPointCloudToData(pointCloud, adjacentPointLocations);

        // 4. Collect error statistics
        s_t pointCloudError = 0.0;
        for (int j = 0; j < adjacentPointLocations.size(); j++)
        {
          pointCloudError
              += (adjacentPointLocations[j] - transformed.col(j)).norm();
        }
        pointCloudError /= adjacentPointLocations.size();
        if (logOutput)
        {
          std::cout << "Joint center " << joint->name
                    << " point cloud reconstruction error: " << pointCloudError
                    << "m" << std::endl;
        }
        totalMarkerError += pointCloudError;
        count++;

        // 5. If the point cloud is co-planar (or very close to it), then
        // there's ambiguity about which side of the plane to place the joint
        // center. We'll check co-planarity on the neutral skeleton also, to
        // avoid having noise in the real marker data fool us into thinking
        // there isn't a coplanar ambiguity. Then we can also use the neutral
        // skeleton to resolve which side of the plane to place the joint
        // center.
        Eigen::Vector3s jointCenter = transformed.col(dim - 1);
        if (isCoplanar(adjacentPointLocationsInNeutralSkel)
            || isCoplanar(adjacentPointLocations))
        {
          if (logOutput)
          {
            std::cout << "Joint " << joint->name
                      << " has coplanar support, and is therefore ambiguous! "
                         "Resolving ambiguity."
                      << std::endl;
          }
          // If the point cloud is coplanar, then we need to resolve the
          // ambiguity about which side of the plane to place the joint center.
          // We do this by checking which side of the plane the joint center is
          // on in the neutral pose, and ensuring that the joint center is on
          // the same side of the plane in the reconstructed pose.
          jointCenter = ensureOnSameSideOfPlane(
              adjacentPointLocationsInNeutralSkel,
              neutralSkelJointCenterWorldPositionsMap[joint->name],
              adjacentPointLocations,
              jointCenter);
        }

        solvedJointCenters[joint->name] = jointCenter;
      }
      if (solvedJointCenters.size() == lastSolvedJointCenters.size())
        break;
      lastSolvedJointCenters = solvedJointCenters;
    }
    mJointCenters.push_back(lastSolvedJointCenters);
  }
  return totalMarkerError / count;
}

//==============================================================================
/// This first finds an approximate rigid body trajectory for each body
/// that has at least 3 markers on it, and then sets up and solves a linear
/// system of equations for each joint to find the pair of offsets in the
/// adjacent body nodes that results in the least offset between the joint
/// centers.
s_t IKInitializer::closedFormPivotFindingJointCenterSolver(bool logOutput)
{
  // 0. Ensure that we've got enough space in our joint centers vector
  while (mJointCenters.size() < mMarkerObservations.size())
  {
    mJointCenters.push_back(std::map<std::string, Eigen::Vector3s>());
  }
  while (mJointAxisDirs.size() < mMarkerObservations.size())
  {
    mJointAxisDirs.push_back(std::map<std::string, Eigen::Vector3s>());
  }

  // 1. Find all the bodies with at least 3 markers on them
  std::map<std::string, int> bodyMarkerCounts;
  for (auto& marker : mMarkers)
  {
    for (auto& stackedBody : mStackedBodies)
    {
      if (std::find(
              stackedBody->bodies.begin(),
              stackedBody->bodies.end(),
              marker.first)
          != stackedBody->bodies.end())
      {
        if (bodyMarkerCounts.count(stackedBody->name) == 0)
          bodyMarkerCounts[stackedBody->name] = 0;
        bodyMarkerCounts[stackedBody->name]++;
        break;
      }
    }
  }

  // 2. Find all the joints with bodies on both sides that have 3 markers on
  // them, and keep track of all adjacent bodies.
  std::vector<std::shared_ptr<struct StackedJoint>> jointsToSolveWithPivots;
  std::vector<std::shared_ptr<struct StackedJoint>>
      jointsToSolveWithChangPollard;
  std::vector<std::shared_ptr<struct StackedBody>> bodiesToFindTransformsFor;
  for (int i = 0; i < mStackedJoints.size(); i++)
  {
    std::shared_ptr<struct StackedJoint> joint = mStackedJoints[i];
    if (joint->parentBody == nullptr)
      continue;

    if (bodyMarkerCounts[joint->parentBody->name] >= 3
        && bodyMarkerCounts[joint->childBody->name] >= 3)
    {
      jointsToSolveWithPivots.push_back(joint);
    }
    else if (
        (bodyMarkerCounts[joint->parentBody->name] >= 3
         || bodyMarkerCounts[joint->childBody->name] >= 3)
        && (bodyMarkerCounts[joint->parentBody->name] > 0
            && bodyMarkerCounts[joint->childBody->name] > 0))
    {
      jointsToSolveWithChangPollard.push_back(joint);
    }
    // Keep track of bodies with enough markers, so we can solve for their
    // transforms over time
    if (bodyMarkerCounts[joint->parentBody->name] >= 3)
    {
      if (std::find(
              bodiesToFindTransformsFor.begin(),
              bodiesToFindTransformsFor.end(),
              joint->parentBody)
          == bodiesToFindTransformsFor.end())
      {
        bodiesToFindTransformsFor.push_back(joint->parentBody);
      }
    }
    if (bodyMarkerCounts[joint->childBody->name] >= 3)
    {
      if (std::find(
              bodiesToFindTransformsFor.begin(),
              bodiesToFindTransformsFor.end(),
              joint->childBody)
          == bodiesToFindTransformsFor.end())
      {
        bodiesToFindTransformsFor.push_back(joint->childBody);
      }
    }
  }

  (void)logOutput;
  // 3. Find the approximate rigid body trajectory for each body. The original
  // transform doesn't matter, since we're just looking for the relative
  // transform between the two bodies. So, arbitrarily, we choose the first
  // timestep where 3 markers are observed as the identity transform.
  std::map<std::string, std::map<int, Eigen::Isometry3s>> bodyTrajectories;
  for (std::shared_ptr<struct StackedBody> body : bodiesToFindTransformsFor)
  {
    // 3.1. Collect the names of the markers we're attached to
    std::vector<std::string> attachedMarkers;
    for (int i = 0; i < mMarkers.size(); i++)
    {
      auto marker = mMarkers[i];
      if (std::find(body->bodies.begin(), body->bodies.end(), marker.first)
          != body->bodies.end())
      {
        attachedMarkers.push_back(mMarkerNames[i]);
      }
    }
    // Go through all the timesteps, solving for relative transforms on the
    // timesteps that we can.
    std::map<int, Eigen::Isometry3s> bodyTrajectory;
    std::vector<std::string> visibleMarkersCloud;
    std::vector<Eigen::Vector3s> visibleMarkerCloudIdentityTransform;
    bool foundFirstFrame = false;
    for (int t = 0; t < mMarkerObservations.size(); t++)
    {
      // 3.2. We first search through all the frames to find the first frame
      // where there are at least 3 markers visible on the body. Call this the
      // identity transform.
      if (!foundFirstFrame)
      {
        visibleMarkersCloud.clear();
        visibleMarkerCloudIdentityTransform.clear();
        for (std::string marker : attachedMarkers)
        {
          if (mMarkerObservations[t].count(marker))
          {
            visibleMarkersCloud.push_back(marker);
            visibleMarkerCloudIdentityTransform.push_back(
                mMarkerObservations[t][marker]);
          }
        }
        if (visibleMarkersCloud.size() >= 3)
        {
          foundFirstFrame = true;
          bodyTrajectory[t] = Eigen::Isometry3s::Identity();
        }
      }
      // 3.3. Once we have the identity transform, we can solve for the relative
      // transform of subsequent frames, if enough markers are visible.
      else
      {
        std::vector<Eigen::Vector3s> identityMarkerCloud;
        std::vector<Eigen::Vector3s> currentMarkerCloud;
        std::vector<s_t> weights;
        for (int i = 0; i < visibleMarkersCloud.size(); i++)
        {
          std::string marker = visibleMarkersCloud[i];
          if (mMarkerObservations[t].count(marker))
          {
            identityMarkerCloud.push_back(
                visibleMarkerCloudIdentityTransform[i]);
            currentMarkerCloud.push_back(mMarkerObservations[t][marker]);
            weights.push_back(1.0);
          }
        }

        // 3.4. If we have enough markers, we can solve for the relative
        // transform at this frame
        if (identityMarkerCloud.size() >= 3)
        {
          bodyTrajectory[t] = getPointCloudToPointCloudTransform(
              identityMarkerCloud, currentMarkerCloud, weights);
        }
      }
    }
    bodyTrajectories[body->name] = bodyTrajectory;
  }

  // 4. Now we can solve for the relative transforms of each joint with 3+
  // markers on BOTH SIDES using pivots, by setting up a linear system of
  // equations where the unknowns are the relative transforms from each body to
  // the joint center, and the constraints are that those map to the same
  // location in world space on each timestep.
  s_t avgJointCenterError = 0.0;
  int solvedJoints = 0;
  for (std::shared_ptr<struct StackedJoint> joint : jointsToSolveWithPivots)
  {
    // At this point, the parent body node shouldn't be null
    assert(joint->parentBody != nullptr);
    std::string parentBodyName = joint->parentBody->name;
    std::string childBodyName = joint->childBody->name;

    // 4.1. Collect all the usable timesteps where transforms of parent and
    // child are both known
    std::vector<int> usableTimesteps;
    for (int t = 0; t < mMarkerObservations.size(); t++)
    {
      if (bodyTrajectories[parentBodyName].count(t)
          && bodyTrajectories[childBodyName].count(t))
      {
        usableTimesteps.push_back(t);
      }
    }
    if (usableTimesteps.size() < 5)
    {
      // Insufficient sample to solve for joint center
      continue;
    }

    // 4.2. Compute the average offset of the joint in the parent and child
    // frames, if we've already done some sort of solve to estimate the joint
    // center. We'll center our solution around that previous estimate.
    Eigen::Vector3s parentAvgOffset = Eigen::Vector3s::Zero();
    Eigen::Vector3s childAvgOffset = Eigen::Vector3s::Zero();
    int observationCount = 0;
    for (int t : usableTimesteps)
    {
      if (mJointCenters[t].count(joint->name))
      {
        parentAvgOffset += bodyTrajectories[parentBodyName][t].inverse()
                           * mJointCenters[t][joint->name];
        childAvgOffset += bodyTrajectories[childBodyName][t].inverse()
                          * mJointCenters[t][joint->name];
        observationCount++;
      }
    }
    if (observationCount > 0)
    {
      parentAvgOffset /= observationCount;
      childAvgOffset /= observationCount;
    }

    // 4.3. Subsample timesteps if necessary to keep problem size reasonable
    const int maxSamples = 500;
    if (usableTimesteps.size() > maxSamples)
    {
      std::vector<int> sampledIndices
          = math::evenlySpacedTimesteps(usableTimesteps.size(), maxSamples);
      std::vector<int> sampledTimesteps;
      for (int index : sampledIndices)
      {
        sampledTimesteps.push_back(usableTimesteps[index]);
      }
      usableTimesteps = sampledTimesteps;
    }

    // 4.4. Setup the linear system of equations
    int cols = 6;
    int rows = usableTimesteps.size() * 3;

    Eigen::MatrixXs A = Eigen::MatrixXs::Zero(rows, cols);
    Eigen::VectorXs b = Eigen::VectorXs::Zero(rows);
    for (int i = 0; i < usableTimesteps.size(); i++)
    {
      Eigen::Isometry3s parentTransform
          = bodyTrajectories[parentBodyName][usableTimesteps[i]];
      Eigen::Isometry3s childTransform
          = bodyTrajectories[childBodyName][usableTimesteps[i]];
      A.block<3, 3>(i * 3, 0) = parentTransform.linear();
      A.block<3, 3>(i * 3, 3) = -1 * childTransform.linear();
      b.segment<3>(i * 3)
          = parentTransform.translation() - childTransform.translation();
    }

    Eigen::VectorXs centerAnswerOn = Eigen::VectorXs::Zero(6);
    centerAnswerOn.head<3>() = parentAvgOffset;
    centerAnswerOn.tail<3>() = childAvgOffset;

    Eigen::VectorXs target = Eigen::VectorXs::Zero(rows);
    auto svd = A.bdcSvd(Eigen::ComputeThinU | Eigen::ComputeThinV);
    Eigen::VectorXs offsets
        = svd.solve(target - b - A * centerAnswerOn) + centerAnswerOn;
    assert(offsets.size() == 6);

    // 4.4. Decode the results of our solution into average values
    Eigen::Vector3s parentOffset = offsets.segment<3>(0);
    Eigen::Vector3s childOffset = offsets.segment<3>(3);

    s_t error = 0.0;
    for (int i = 0; i < usableTimesteps.size(); i++)
    {
      Eigen::Isometry3s parentTransform = bodyTrajectories[parentBodyName][i];
      Eigen::Isometry3s childTransform = bodyTrajectories[childBodyName][i];
      Eigen::Vector3s parentWorldCenter = parentTransform * parentOffset;
      Eigen::Vector3s childWorldCenter = childTransform * childOffset;
      Eigen::Vector3s jointCenter
          = (parentWorldCenter + childWorldCenter) / 2.0;
      error += (parentWorldCenter - jointCenter).norm();
      error += (childWorldCenter - jointCenter).norm();

      // 4.4.1. Record the result
      mJointCenters[usableTimesteps[i]][joint->name] = jointCenter;
    }

    // 4.5. Check if we're on an axis, which will show up if there's a 0
    // singular value.
    Eigen::VectorXs singularValues = svd.singularValues();
    Eigen::MatrixXs V = svd.matrixV();
#ifndef NDEBUG
    Eigen::MatrixXs A_reconstructed
        = svd.matrixU() * singularValues.asDiagonal() * V.transpose();
    assert((A - A_reconstructed).norm() < 1e-8);
#endif
    // We normalize the singular values to be agnostic to the size of the data
    // we're dealing with in the original matrix.
    Eigen::VectorXs normalizedSingularValues = singularValues.normalized();
    if (logOutput)
    {
      std::cout << "Joint \"" << joint->name
                << "\" smallest normalized singular value is "
                << normalizedSingularValues(5) << std::endl;
    }
    if (std::abs(normalizedSingularValues(5)) < 1e-2)
    {
      if (logOutput)
      {
        std::cout << "Joint \"" << joint->name << "\" is an axis joint!"
                  << std::endl;
      }
      // 4.5.1. We found an axis! Record it.
      Eigen::Vector6s nullSpace = V.col(5);
      Eigen::Vector3s parentLocalAxis = nullSpace.segment<3>(0).normalized();
      Eigen::Vector3s childLocalAxis = nullSpace.segment<3>(3).normalized();

      // 4.5.2. Transform the axis direction back to world space
      for (int i = 0; i < usableTimesteps.size(); i++)
      {
        Eigen::Isometry3s parentTransform = bodyTrajectories[parentBodyName][i];
        Eigen::Isometry3s childTransform = bodyTrajectories[childBodyName][i];
        Eigen::Vector3s parentWorldAxis
            = parentTransform.linear() * parentLocalAxis;
        Eigen::Vector3s childWorldAxis
            = childTransform.linear() * childLocalAxis;
        Eigen::Vector3s jointAxis = (parentWorldAxis + childWorldAxis) / 2.0;
        // 4.5.3. Record the result
        mJointAxisDirs[usableTimesteps[i]][joint->name]
            = jointAxis.normalized();
      }
    }

    if (logOutput)
    {
      std::cout << "Solved for joint \"" << joint->name
                << "\" with a linear system on body transforms with "
                << usableTimesteps.size() << " samples, error: " << error << "m"
                << std::endl;
    }

    error /= usableTimesteps.size() * 2;
    avgJointCenterError += error;
    solvedJoints++;
  }

  // 5. Now we can solve for the relative transforms of each joint with 3+
  // markers on just ONE SIDE, and 1-2 markers on the other using
  // ChangPollard2006 (which does a better job handling small ranges of motion
  // than vanilla least-squares)
  for (std::shared_ptr<struct StackedJoint> joint :
       jointsToSolveWithChangPollard)
  {
    // 5.1. Figure out which body is going to be the reference frame, and which
    // body has only a few markers to solve with
    std::shared_ptr<struct StackedBody> anchorBody = nullptr;
    std::shared_ptr<struct StackedBody> otherBody = nullptr;
    if (bodyMarkerCounts[joint->parentBody->name] >= 3)
    {
      assert(bodyMarkerCounts[joint->childBody->name] < 3);
      assert(bodyMarkerCounts[joint->childBody->name] > 0);
      anchorBody = joint->parentBody;
      otherBody = joint->childBody;
    }
    else
    {
      assert(bodyMarkerCounts[joint->parentBody->name] < 3);
      assert(bodyMarkerCounts[joint->parentBody->name] > 0);
      assert(bodyMarkerCounts[joint->childBody->name] >= 3);
      anchorBody = joint->childBody;
      otherBody = joint->parentBody;
    }

    // 5.2. Work out which markers will be moving around on the other body
    std::vector<std::string> movingMarkers;
    for (int i = 0; i < mMarkers.size(); i++)
    {
      if (std::find(
              otherBody->bodies.begin(),
              otherBody->bodies.end(),
              mMarkers[i].first)
          != otherBody->bodies.end())
      {
        movingMarkers.push_back(mMarkerNames[i]);
      }
    }

    // 5.3. Go through and transform all the markers into the anchor body's
    // frame.
    std::map<std::string, std::vector<Eigen::Vector3s>> anchorBodyMarkerClouds;
    for (auto& pair : bodyTrajectories[anchorBody->name])
    {
      int t = pair.first;
      Eigen::Isometry3s anchorTransform = pair.second;
      for (std::string movingMarker : movingMarkers)
      {
        if (mMarkerObservations[t].count(movingMarker))
        {
          if (anchorBodyMarkerClouds.count(movingMarker) == 0)
          {
            anchorBodyMarkerClouds[movingMarker]
                = std::vector<Eigen::Vector3s>();
          }
          anchorBodyMarkerClouds[movingMarker].push_back(
              anchorTransform.inverse() * mMarkerObservations[t][movingMarker]);
        }
      }
    }
    std::vector<std::vector<Eigen::Vector3s>> markerTraces;
    for (auto& pair : anchorBodyMarkerClouds)
    {
      markerTraces.push_back(pair.second);
    }

    // 5.4. Get the local joint center in the anchor body
    Eigen::Vector3s jointCenter
        = getChangPollard2006JointCenterMultiMarker(markerTraces);

    // 5.5. Transform the result back into world space, and record them
    for (auto& pair : bodyTrajectories[anchorBody->name])
    {
      int t = pair.first;
      Eigen::Isometry3s anchorTransform = pair.second;
      Eigen::Vector3s jointWorldCenter = anchorTransform * jointCenter;
      mJointCenters[t][joint->name] = jointWorldCenter;
    }

    // 5.6. Get the axis direction in the anchor body, along with the
    // corresponding singular value
    std::pair<Eigen::Vector3s, s_t> axisDirAndSingularValue
        = gamageLasenby2002AxisFit(markerTraces);
    Eigen::Vector3s jointAxisDir = axisDirAndSingularValue.first;
    s_t singularValue = axisDirAndSingularValue.second;
    if (std::abs(singularValue) < 1e-2)
    {
      // 5.6.1. We found an axis! Record it.
      if (logOutput)
      {
        std::cout << "Joint \"" << joint->name
                  << "\" found a near-zero singular value (" << singularValue
                  << ") in its linear system, so it's an axis joint."
                  << std::endl;
      }
      // 5.6.2. Transform the axis direction back to world space
      for (auto& pair : bodyTrajectories[anchorBody->name])
      {
        int t = pair.first;
        Eigen::Isometry3s anchorTransform = pair.second;
        Eigen::Vector3s jointWorldAxis
            = anchorTransform.linear() * jointAxisDir;
        mJointAxisDirs[t][joint->name] = jointWorldAxis.normalized();
      }
    }

    if (logOutput)
    {
      std::cout << "Solved for joint \"" << joint->name
                << "\" with Chang Pollard 2006" << std::endl;
    }
  }

  avgJointCenterError /= solvedJoints;
  return avgJointCenterError;
}

//==============================================================================
/// For joints where we believe they're floating along an axis, and we have at
/// least one of either the parent or child joint has a known joint center
/// WITHOUT the axis amiguity, we can slide the joint center along the axis
/// until the angle formed by the axis and the known adjacent joint matches
/// what is in the skeleton.
s_t IKInitializer::recenterAxisJointsBasedOnBoneAngles(bool logOutput)
{
  // 0. Save the default pose and scale's version of the joint centers and
  // marker locations, so that we can use it to detect and resolve coplanar
  // ambiguity in subsequent steps.
  Eigen::VectorXs oldPositions = mSkel->getPositions();
  Eigen::VectorXs oldScales = mSkel->getBodyScales();
  mSkel->setPositions(Eigen::VectorXs::Zero(mSkel->getNumDofs()));
  mSkel->setBodyScales(Eigen::VectorXs::Ones(mSkel->getNumBodyNodes() * 3));
  Eigen::VectorXs neutralSkelJointWorldPositions
      = mSkel->getJointWorldPositions(mSkel->getJoints());
  Eigen::VectorXs neutralSkelMarkerWorldPositions
      = mSkel->getMarkerWorldPositions(mMarkers);
  std::map<std::string, Eigen::Vector3s> neutralSkelMarkerWorldPositionsMap;
  for (int i = 0; i < mMarkerNames.size(); i++)
  {
    neutralSkelMarkerWorldPositionsMap[mMarkerNames[i]]
        = neutralSkelMarkerWorldPositions.segment<3>(i * 3);
  }
  std::map<std::string, Eigen::Vector3s>
      neutralSkelJointCenterWorldPositionsMap;
  std::map<std::string, Eigen::Vector3s> neutralSkelJointAxisWorldDirectionsMap;
  for (int i = 0; i < mStackedJoints.size(); i++)
  {
    neutralSkelJointCenterWorldPositionsMap[mStackedJoints[i]->name]
        = getStackedJointCenterFromJointCentersVector(
            mStackedJoints[i], neutralSkelJointWorldPositions);
    neutralSkelJointAxisWorldDirectionsMap[mStackedJoints[i]->name]
        = mStackedJoints[i]
              ->joints[0]
              ->getWorldAxisScrewForPosition(0)
              .head<3>()
              .normalized();
    std::string jointType = mStackedJoints[i]->joints[0]->getType();
    bool isPureRevolute
        = jointType == dynamics::RevoluteJoint::getStaticType()
          || jointType == dynamics::EulerJoint::getStaticType()
          || jointType == dynamics::BallJoint::getStaticType()
          || jointType == dynamics::UniversalJoint::getStaticType();

    // If we're not a pure revolute joint (for example, a CustomJoint driven by
    // evil splines) we need to do some extra work to estimate a useful joint
    // axis in the neutral pose.
    if (!isPureRevolute)
    {
      if (logOutput)
      {
        std::cout << "Joint \"" << mStackedJoints[i]->name
                  << "\" is not a pure revolute joint, so we're going to "
                     "estimate a joint axis based on a sweep of virtual "
                     "markers, and then a Gamage Lasenby axis fit."
                  << std::endl;
      }
      dynamics::Joint* revoluteJoint = mStackedJoints[i]->joints[0];
      std::vector<std::pair<dynamics::BodyNode*, Eigen::Vector3s>> markers;
      for (int i = 0; i < 3; i++)
      {
        markers.push_back(std::make_pair(
            revoluteJoint->getChildBodyNode(), Eigen::Vector3s::Unit(i)));
      }

      s_t min = revoluteJoint->getDof(0)->getPositionLowerLimit();
      s_t max = revoluteJoint->getDof(0)->getPositionUpperLimit();
      int steps = 2000;
      s_t stepSize = (max - min) / (s_t)steps;

      std::vector<std::vector<Eigen::Vector3s>> markerObservationsOverSweep;
      for (int i = 0; i < markers.size(); i++)
      {
        markerObservationsOverSweep.push_back(std::vector<Eigen::Vector3s>());
      }
      for (int i = 0; i < steps; i++)
      {
        s_t pos = min + stepSize * i;
        revoluteJoint->setPosition(0, pos);
        Eigen::VectorXs markerWorldPos
            = mSkel->getMarkerWorldPositions(markers);
        for (int j = 0; j < markers.size(); j++)
        {
          markerObservationsOverSweep[j].push_back(
              markerWorldPos.segment<3>(j * 3));
        }
      }
      // Reset to the neutral pose when we're done
      revoluteJoint->setPosition(0, 0.0);

      Eigen::Vector3s axis
          = gamageLasenby2002AxisFit(markerObservationsOverSweep).first;
      if (logOutput)
      {
        std::cout << "Joint \"" << mStackedJoints[i]->name
                  << "\" found Gamage Lasenby axis in neutral pos: "
                  << axis.transpose() << std::endl;
      }
      neutralSkelJointAxisWorldDirectionsMap[mStackedJoints[i]->name] = axis;
    }
  }
  mSkel->setPositions(oldPositions);
  mSkel->setBodyScales(oldScales);

  // 1. Sort the joints into two categories: "axis joints", where the solver
  // found an axis along which the joint center is ambiguous, and "center
  // joints", where the solver believes it knows the joint center without
  // ambiguity.
  std::vector<std::shared_ptr<struct StackedJoint>> axisJoints;
  std::vector<std::shared_ptr<struct StackedJoint>> centerJoints;
  std::map<std::string, bool> isJointAxis;

  for (std::shared_ptr<struct StackedJoint> joint : mStackedJoints)
  {
    int numCentersObserved = 0;
    int numAxisObserved = 0;
    for (int t = 0; t < mMarkerObservations.size(); t++)
    {
      if (mJointCenters[t].count(joint->name))
      {
        numCentersObserved++;
      }
      if (mJointAxisDirs[t].count(joint->name))
      {
        numAxisObserved++;
      }
    }
    if (numCentersObserved > 0 && numAxisObserved > 0)
    {
      isJointAxis[joint->name] = true;
      if (logOutput)
      {
        std::cout
            << "Joint \"" << joint->name
            << "\" has found axis, we're going to attempt to re-center it."
            << std::endl;
      }
      axisJoints.push_back(joint);
    }
    else if (numCentersObserved > 0)
    {
      isJointAxis[joint->name] = false;
      centerJoints.push_back(joint);
    }
  }

  // 2. We're going to repeatedly look for an axis joint that meets our criteria
  // for "solvability", in order to work out how far along the axis the joint
  // center lies relative to adjacent "center joints". Once we've solved an
  // "axis joint", and adjusted its center location, we'll move it to be a
  // "center joint" and repeat the process until we can't find any more joints
  // to improve.
  (void)logOutput;
  while (true)
  {
    bool anyChanged = false;

    for (int i = 0; i < axisJoints.size(); i++)
    {
      std::shared_ptr<struct StackedJoint> joint = axisJoints[i];

      // 2.1. Find the joints that are not axis joints, that are adjacent to
      // this joint. This includes both parent and as many children as fit the
      // criteria.
      std::vector<std::shared_ptr<struct StackedJoint>> adjacentPointJoints;
      if (joint->parentBody != nullptr
          && !isJointAxis[joint->parentBody->parentJoint->name])
      {
        adjacentPointJoints.push_back(joint->parentBody->parentJoint);
      }
      for (auto& childJoint : joint->childBody->childJoints)
      {
        if (!isJointAxis[childJoint->name])
        {
          adjacentPointJoints.push_back(childJoint);
        }
      }

      // 2.2. On joints where at least one adjacent point has a known joint
      // center, we're going to attempt to go through and solve for the correct
      // spot to put the joint center along the axis.
      //
      // We're going to solve by reference to the ratio of of the triangle's
      // sides formed between the axis and the adjacent joint center(s),
      // measured along the joint axis and perpendicular to it.
      //
      // For example:
      //
      //          adjacent joint
      //           /    |
      //          /     |
      //         /      |
      //        /       | (perpendicular dist)
      //       /        |
      //      /         |
      //   joint ----- axis
      //   (parallel dist)
      //
      // The angle between the axis and the adjacent joint is given by the ratio
      // of the parallel and perpendicular distances. Given an unambiguous
      // perpendicular distance from our solver, and an unambiguous joint axis,
      // we can solve for the distance along the axis by solving for the ratio
      // of the parallel distance to the perpendicular distance.
      if (adjacentPointJoints.size() > 0)
      {
        // 2.2.1. Collect the joint centers and axis direction on the neutral
        // skeleton, so we can determine our ratio of parallel to perpendicular
        // distances in the neutral pose
        const Eigen::Vector3s neutralAxis
            = neutralSkelJointAxisWorldDirectionsMap[joint->name];
        const Eigen::Vector3s neutralJointCenter
            = neutralSkelJointCenterWorldPositionsMap[joint->name];

        for (std::shared_ptr<struct StackedJoint> pointJoint :
             adjacentPointJoints)
        {
          Eigen::Vector3s adjacentNeutralJointCenter
              = neutralSkelJointCenterWorldPositionsMap[pointJoint->name];

          // 2.2.2. Find the joint center distance to the adjacent joint both
          // along the axis, and perpindicular to the axis
          Eigen::Vector3s neutralOffset
              = adjacentNeutralJointCenter - neutralJointCenter;
          s_t neutralDistAlongAxis = neutralOffset.dot(neutralAxis);
          Eigen::Vector3s neutralOffsetParallel
              = neutralDistAlongAxis * neutralAxis;
          Eigen::Vector3s neutralOffsetPerp
              = (neutralOffset - neutralOffsetParallel);
          assert(
              (neutralOffsetParallel + neutralOffsetPerp - neutralOffset).norm()
              < 1e-15);
          assert(
              (neutralJointCenter + neutralOffsetParallel + neutralOffsetPerp
               - adjacentNeutralJointCenter)
                  .norm()
              < 1e-15);
          s_t neutralRatio = neutralDistAlongAxis / neutralOffsetPerp.norm();

          // 2.2.3. Find the intervening body between us and the pin joint, and
          // record it for later disambiguation.
          std::shared_ptr<struct StackedBody> interveningBody = nullptr;
          if (pointJoint->parentBody != nullptr
              && pointJoint->parentBody->parentJoint == joint)
          {
            interveningBody = pointJoint->parentBody;
          }
          else
          {
            assert(joint->parentBody != nullptr);
            assert(joint->parentBody->parentJoint == pointJoint);
            interveningBody = joint->parentBody;
          }
          assert(interveningBody != nullptr);

          if (logOutput)
          {
            std::cout << "Shifting joint \"" << joint->name
                      << "\" along axis with help of joint \""
                      << pointJoint->name << "\" with neutral ratio "
                      << neutralRatio << std::endl;
            std::cout << "  -> Joint neutral center: "
                      << neutralJointCenter.transpose() << std::endl;
            std::cout << "  -> Joint neutral axis: " << neutralAxis.transpose()
                      << std::endl;
            std::cout << "  -> Adjacent neutral center: "
                      << adjacentNeutralJointCenter.transpose() << std::endl;
          }

          // 2.2.4. Now we're going to go through all the timesteps and grab the
          // ratio in practice, and then use that to solve for the two joint
          // center offsets possible along the axis.
          for (int t = 0; t < mMarkerObservations.size(); t++)
          {
            if (mJointCenters[t].count(joint->name)
                && mJointAxisDirs[t].count(joint->name)
                && mJointCenters[t].count(pointJoint->name))
            {
              // Get the joint center and axis direction in world space
              Eigen::Vector3s jointCenter = mJointCenters[t][joint->name];
              Eigen::Vector3s jointAxis = mJointAxisDirs[t][joint->name];

              // Get the adjacent joint center in world space
              Eigen::Vector3s adjacentJointCenter
                  = mJointCenters[t][pointJoint->name];

              // 2.2.5. We're going to make sure that our axis is pointed in the
              // correct direction, by aligning the point cloud of visible
              // markers on the interveningBody with the neutral pose, and
              // checking the direction of the neutral axis under the resulting
              // transformation.
              std::vector<Eigen::Vector3s> neutralMarkerLocations;
              std::vector<Eigen::Vector3s> worldMarkerLocations;
              std::vector<s_t> weights;
              for (int j = 0; j < mMarkers.size(); j++)
              {
                if (std::find(
                        interveningBody->bodies.begin(),
                        interveningBody->bodies.end(),
                        mMarkers[j].first)
                    != interveningBody->bodies.end())
                {
                  if (mMarkerObservations[t].count(mMarkerNames[j]))
                  {
                    neutralMarkerLocations.push_back(
                        neutralSkelMarkerWorldPositionsMap[mMarkerNames[j]]);
                    worldMarkerLocations.push_back(
                        mMarkerObservations[t][mMarkerNames[j]]);
                    weights.push_back(1.0);
                  }
                }
              }
              Eigen::Isometry3s neutralToWorldTransform
                  = getPointCloudToPointCloudTransform(
                      neutralMarkerLocations, worldMarkerLocations, weights);
              Eigen::Vector3s neutralAxisInWorld
                  = neutralToWorldTransform.linear() * neutralAxis;
              // Ensure that the joint axis is pointed in the same direction as
              // the neutral axis
              if (neutralAxisInWorld.dot(jointAxis) < 0)
              {
                jointAxis *= -1;
              }
              s_t recoveredAxisSimilarity = neutralAxisInWorld.dot(jointAxis);
              if (logOutput)
              {
                std::cout
                    << "Recovered world axis similarity (after flipping): "
                    << recoveredAxisSimilarity << std::endl;
              }

              // 2.2.6. Now that we've corrected the axis direction, the joint
              // center can be found unambiguously
              // Project `other_point` onto the line.
              // Eigen::Vector3d projection = jointCenter +
              // (jointAxis.dot(adjacentJointCenter - jointCenter) /
              // jointAxis.dot(jointAxis)) * jointAxis;
              Eigen::Vector3s offset = adjacentJointCenter - jointCenter;
              s_t distAlongAxis = offset.dot(jointAxis);
              Eigen::Vector3s offsetParallel = distAlongAxis * jointAxis;
              Eigen::Vector3s offsetPerp = (offset - offsetParallel);

              Eigen::Vector3s closestPointOnAxis = jointCenter + offsetParallel;
              assert(
                  (closestPointOnAxis + offsetPerp - adjacentJointCenter).norm()
                  < 1e-15);
              s_t orthogonalError
                  = (closestPointOnAxis - adjacentJointCenter).dot(jointAxis);
              (void)orthogonalError;
              assert(orthogonalError < 1e-15);

              Eigen::Vector3s correctedJointCenter
                  = closestPointOnAxis
                    - jointAxis * offsetPerp.norm() * neutralRatio;

#ifndef NDEBUG
              // Check that the ratio is now preserved
              Eigen::Vector3s recoveredOffset
                  = adjacentJointCenter - correctedJointCenter;
              s_t recoveredDistAlongAxis = recoveredOffset.dot(jointAxis);
              Eigen::Vector3s recoveredOffsetParallel
                  = recoveredDistAlongAxis * jointAxis;
              Eigen::Vector3s recoveredOffsetPerp
                  = (recoveredOffset - recoveredOffsetParallel);
              assert(
                  (recoveredOffsetParallel + recoveredOffsetPerp
                   - recoveredOffset)
                      .norm()
                  < 1e-15);
              assert(
                  (correctedJointCenter + recoveredOffsetParallel
                   + recoveredOffsetPerp - adjacentJointCenter)
                      .norm()
                  < 1e-15);
              s_t recoveredRatio
                  = recoveredDistAlongAxis / recoveredOffsetPerp.norm();
              assert(abs(recoveredRatio - neutralRatio) < 1e-8);
#endif

              s_t movedDist
                  = (correctedJointCenter - mJointCenters[t][joint->name])
                        .norm();
              if (movedDist < 0.10)
              {
                if (logOutput)
                {
                  std::cout << "Axis aligned joint center for joint \""
                            << joint->name << " by shifting " << movedDist
                            << "m: [" << correctedJointCenter.transpose()
                            << "] from ["
                            << mJointCenters[t][joint->name].transpose() << "]"
                            << std::endl;
                }
                mJointCenters[t][joint->name] = correctedJointCenter;
              }
              else
              {
                if (logOutput)
                {
                  std::cout << "Axis aligned joint center for joint \""
                            << joint->name << " wants to shift " << movedDist
                            << "m which is suspiciously large, and so we're "
                               "ignoring it."
                            << std::endl;
                }
              }
            }
          }
        }

        // 2.2.6. Now we can mark this joint as having a reliable joint center,
        // so we can use it as support in the next iteration of the algorithm.
        centerJoints.push_back(joint);
        axisJoints.erase(axisJoints.begin() + i);
        isJointAxis[joint->name] = false;
        anyChanged = true;
        break;
      }
    }

    if (!anyChanged)
    {
      break;
    }
  }

  return 0.0;
}

//==============================================================================
/// This uses the current guesses for the joint centers to re-estimate the
/// bone sizes (based on distance between joint centers) and then use that to
/// get the group scale vector. This also uses the joint centers to estimate
/// the body positions.
void IKInitializer::estimateGroupScalesClosedForm(bool log)
{
  Eigen::VectorXs originalPose = mSkel->getPositions();
  Eigen::VectorXs originalBodyScales = mSkel->getBodyScales();
  mSkel->setBodyScales(Eigen::VectorXs::Ones(mSkel->getNumBodyNodes() * 3));
  mSkel->setPositions(Eigen::VectorXs::Zero(mSkel->getNumDofs()));

  Eigen::VectorXs jointWorldPositions
      = mSkel->getJointWorldPositions(mSkel->getJoints());

  // 1. Estimate the bone sizes from the joint centers

  // 1.1. Get estimated joint to joint distances from our current joint center
  // guesses
  std::map<std::string, std::map<std::string, s_t>>
      estimatedJointToJointDistances = estimateJointToJointDistances();
  // 1.2. Find a scale for all the bodies that we can
  std::map<std::string, Eigen::Vector3s> bodyScales;
  std::map<std::string, Eigen::Vector3s> bodyScaleWeights;
  for (std::shared_ptr<struct StackedBody> bodyNode : mStackedBodies)
  {
    // 1.2.1. Collect joints adjacent to this body that we can potentially use
    // to help scale
    std::vector<std::shared_ptr<struct StackedJoint>> adjacentJoints;
    adjacentJoints.push_back(bodyNode->parentJoint);
    for (int i = 0; i < bodyNode->childJoints.size(); i++)
    {
      adjacentJoints.push_back(bodyNode->childJoints[i]);
    }

    // 1.2.2. Find which pairs of joints (expressed as locations in the local
    // frame of the body), if any, have estimated distances between them
    std::vector<std::tuple<std::string, std::string, s_t>> jointPairs;
    std::map<std::string, Eigen::Vector3s> jointsInLocalSpace;
    for (int i = 0; i < adjacentJoints.size(); i++)
    {
      for (int j = i + 1; j < adjacentJoints.size(); j++)
      {
        std::shared_ptr<struct StackedJoint> joint1 = adjacentJoints[i];
        std::shared_ptr<struct StackedJoint> joint2 = adjacentJoints[j];
        if (estimatedJointToJointDistances.count(joint1->name) > 0
            && estimatedJointToJointDistances[joint1->name].count(joint2->name)
                   > 0)
        {
          s_t dist = estimatedJointToJointDistances[joint1->name][joint2->name];
          Eigen::Vector3s joint1WorldPos
              = getStackedJointCenterFromJointCentersVector(
                  joint1, jointWorldPositions);
          Eigen::Vector3s joint2WorldPos
              = getStackedJointCenterFromJointCentersVector(
                  joint2, jointWorldPositions);
          Eigen::Vector3s joint1LocalPos
              = bodyNode->bodies[0]->getWorldTransform().inverse()
                * joint1WorldPos;
          jointsInLocalSpace[joint1->name] = joint1LocalPos;
          Eigen::Vector3s joint2LocalPos
              = bodyNode->bodies[0]->getWorldTransform().inverse()
                * joint2WorldPos;
          jointsInLocalSpace[joint2->name] = joint2LocalPos;
          jointPairs.emplace_back(joint1->name, joint2->name, dist);
        }
      }
    }

    // 1.2.3. If we have a single joint pair, then we can use that to scale the
    // body
    if (jointPairs.size() == 1)
    {
      std::string joint1Name = std::get<0>(jointPairs[0]);
      std::string joint2Name = std::get<1>(jointPairs[0]);
      s_t desiredDist = std::get<2>(jointPairs[0]);
      s_t currentDist
          = (jointsInLocalSpace[joint1Name] - jointsInLocalSpace[joint2Name])
                .norm();
      s_t scale = desiredDist / currentDist;
      if (log)
      {
        std::cout << "Scaling " << bodyNode->name << " by (" << scale
                  << ") based on " << joint1Name << " and " << joint2Name
                  << std::endl;
      }
      for (auto* body : bodyNode->bodies)
      {
        bodyScales[body->getName()] = Eigen::Vector3s(scale, scale, scale);
        bodyScaleWeights[body->getName()]
            = Eigen::Vector3s::Ones() * currentDist * currentDist;
      }
    }
    // 1.2.4. If we have multiple joint pairs, then we can use those to scale
    // along multiple axis at once.
    else if (jointPairs.size() > 1)
    {
      assert(jointsInLocalSpace.size() > 0);
      std::map<std::string, int> jointToIndex;
      std::vector<Eigen::Vector3s> jointsInLocalSpaceVector;
      std::vector<std::string> jointNames;
      for (auto& pair : jointsInLocalSpace)
      {
        jointToIndex[pair.first] = jointToIndex.size();
        jointNames.push_back(pair.first);
        jointsInLocalSpaceVector.push_back(pair.second);
      }
      assert(jointsInLocalSpaceVector.size() > 0);

      // Default the square distances to the current distances, in case we're
      // missing some pairs
      Eigen::MatrixXs squaredDistances = Eigen::MatrixXs::Zero(
          jointsInLocalSpace.size(), jointsInLocalSpace.size());
      for (auto& pair1 : jointsInLocalSpace)
      {
        for (auto& pair2 : jointsInLocalSpace)
        {
          squaredDistances(jointToIndex[pair1.first], jointToIndex[pair2.first])
              = (pair1.second - pair2.second).squaredNorm();
        }
      }

      // Now overwrite the distances we have with the estimated distances
      for (auto& pair : jointPairs)
      {
        std::string joint1Name = std::get<0>(pair);
        std::string joint2Name = std::get<1>(pair);
        s_t desiredDist = std::get<2>(pair);
        squaredDistances(jointToIndex[joint1Name], jointToIndex[joint2Name])
            = desiredDist * desiredDist;
        squaredDistances(jointToIndex[joint2Name], jointToIndex[joint1Name])
            = desiredDist * desiredDist;
      }

      // Now we can run MDS to get the point cloud
      Eigen::MatrixXs rawPointCloud
          = getPointCloudFromDistanceMatrix(squaredDistances);

      // Now we can map the point cloud back to the original locations
      Eigen::MatrixXs pointCloud
          = mapPointCloudToData(rawPointCloud, jointsInLocalSpaceVector);

      // If any of the joints are at the origin in our local body coordinates,
      // we want to re-center the target point cloud so that the same point is
      // at its origin, or else scaling won't work.
      int vectorAtOrigin = -1;
      for (int i = 0; i < jointsInLocalSpaceVector.size(); i++)
      {
        if (jointsInLocalSpaceVector[i].isZero())
        {
          vectorAtOrigin = i;
        }
      }
      if (vectorAtOrigin != -1)
      {
        Eigen::Vector3s origin = pointCloud.col(vectorAtOrigin);
        for (int i = 0; i < pointCloud.cols(); i++)
        {
          pointCloud.col(i) -= origin;
        }
      }

      // Compute axis scales as the average ratio of the columns of `pointCloud`
      // to `jointsInLocalSpaceVector`
      Eigen::Vector3s axisScales = Eigen::Vector3s::Zero();
      Eigen::Vector3s axisCounts = Eigen::Vector3s::Zero();
      Eigen::Vector3s axisLengthSum = Eigen::Vector3s::Zero();
      for (int i = 0; i < jointsInLocalSpaceVector.size(); i++)
      {
        axisLengthSum += jointsInLocalSpaceVector[i].cwiseProduct(
            jointsInLocalSpaceVector[i]);
        for (int axis = 0; axis < 3; axis++)
        {
          if (std::abs(jointsInLocalSpaceVector[i](axis)) > 0)
          {
            axisScales(axis)
                += pointCloud(axis, i) / jointsInLocalSpaceVector[i](axis);
            axisCounts(axis) += 1;
          }
        }
      }
      int observedAnyAxis = -1;
      for (int axis = 0; axis < 3; axis++)
      {
        if (axisCounts(axis) > 0)
        {
          axisScales(axis) /= axisCounts(axis);
          observedAnyAxis = axis;
        }
      }
      if (observedAnyAxis != -1)
      {
        for (int axis = 0; axis < 3; axis++)
        {
          if (axisCounts(axis) == 0)
          {
            // We have no info about this axis, so assume the scale is whatever
            // the observed scales were
            axisScales(axis) = axisScales(observedAnyAxis);
            if (log)
            {
              std::cout
                  << "Body " << bodyNode->name
                  << " has no observations with non-zero-length along axis="
                  << axis << std::endl;
            }
          }
        }
      }
      else
      {
        std::cout << "Body " << bodyNode->name
                  << " has no observations with non-zero-length along any "
                     "axis! This is probably a bug."
                  << std::endl;
        axisScales.setConstant(1.0);
      }

      if (log)
      {
        std::cout << "Scaling " << bodyNode->name << " by ("
                  << axisScales.transpose() << ") based on "
                  << jointPairs.size() << " pairs of joints" << std::endl;
        for (auto& pair : jointPairs)
        {
          std::cout << "  Pair " << std::get<0>(pair) << " and "
                    << std::get<1>(pair) << " with distance "
                    << std::get<2>(pair) << std::endl;
        }
        for (int i = 0; i < jointsInLocalSpaceVector.size(); i++)
        {
          Eigen::Vector3s target = pointCloud.col(i);
          Eigen::Vector3s scaled
              = jointsInLocalSpaceVector[i].cwiseProduct(axisScales);
          std::cout << "  Joint " << jointNames[i]
                    << " reconstruction error = " << (target - scaled).norm()
                    << ": target " << target.transpose() << " vs scaled "
                    << scaled.transpose() << std::endl;
        }
      }

      for (auto* body : bodyNode->bodies)
      {
        bodyScales[body->getName()] = axisScales;
        bodyScaleWeights[body->getName()] = axisLengthSum;
      }
    }
  }
  // 1.3. Scale any remaining bodies to a default scale based on the height, if
  // known
  Eigen::Vector3s defaultScales = Eigen::Vector3s::Ones();
  Eigen::Vector3s defaultScaleWeights = Eigen::Vector3s::Ones() * 0.01;

  if (mModelHeightM > 0)
  {
    s_t defaultHeight
        = mSkel->getHeight(Eigen::VectorXs::Zero(mSkel->getNumDofs()));
    if (defaultHeight > 0)
    {
      s_t ratio = mModelHeightM / defaultHeight;
      defaultScales = Eigen::Vector3s(ratio, ratio, ratio);
    }
  }

  for (dynamics::BodyNode* bodyNode : mSkel->getBodyNodes())
  {
    if (bodyScales.count(bodyNode->getName()) == 0)
    {
      bodyScales[bodyNode->getName()] = defaultScales;
      bodyScaleWeights[bodyNode->getName()] = defaultScaleWeights;
    }
  }
  // 1.4. Apply all the scalings to bodies, taking weighted averages over scale
  // groups
  for (auto& group : mSkel->getBodyScaleGroups())
  {
    Eigen::Vector3s scaleAvg = Eigen::Vector3s::Zero();
    Eigen::Vector3s weightsSum = Eigen::Vector3s::Zero();

    std::cout << "Scaling group:" << std::endl;
    for (auto* body : group.nodes)
    {
      // Prevent divide-by-zero errors
      Eigen::Vector3s weight = bodyScaleWeights[body->getName()];
      for (int axis = 0; axis < 3; axis++)
      {
        if (weight(axis) <= 0)
          weight(axis) = 1e-5;
      }

      std::cout << "  " << body->getName() << " = "
                << bodyScales[body->getName()].transpose() << " with weight "
                << weight.transpose() << std::endl;
      scaleAvg += bodyScales[body->getName()].cwiseProduct(weight);
      weightsSum += weight;
    }
    scaleAvg = scaleAvg.cwiseQuotient(weightsSum);
    std::cout << "  -> weighted avg = " << scaleAvg.transpose() << std::endl;
    for (auto* body : group.nodes)
    {
      body->setScale(scaleAvg);
    }
  }
  // 1.5. Ensure that the scaling is symmetric across groups, by condensing into
  // the group scales vector and then re-setting the body scales from that
  // vector.
  mGroupScales = mSkel->getGroupScales();
  mSkel->setGroupScales(mGroupScales);

  mSkel->setBodyScales(originalBodyScales);
  mSkel->setPositions(originalPose);
}

//==============================================================================
/// WARNING: You must have already called estimateGroupScalesClosedForm()!
/// This uses the joint centers to estimate the body positions.
s_t IKInitializer::estimatePosesClosedForm(bool logOutput)
{
  Eigen::VectorXs originalPose = mSkel->getPositions();
  mSkel->setPositions(Eigen::VectorXs::Zero(mSkel->getNumDofs()));

  // Compute the joint world positions
  Eigen::VectorXs jointWorldPositions
      = mSkel->getJointWorldPositions(mSkel->getJoints());

  // 1. We'll want to use annotations on top of our stacked bodies, so construct
  // structs to make that easier.
  std::vector<AnnotatedStackedBody> annotatedGroups;
  for (int i = 0; i < mStackedBodies.size(); i++)
  {
    AnnotatedStackedBody group;
    group.stackedBody = mStackedBodies[i];
    annotatedGroups.push_back(group);
  }
  // 1.1.1. Compute body relative transforms
  for (int i = 0; i < annotatedGroups.size(); i++)
  {
    for (int j = 0; j < annotatedGroups[i].stackedBody->bodies.size(); j++)
    {
      annotatedGroups[i].relativeTransforms.push_back(
          annotatedGroups[i]
              .stackedBody->bodies[0]
              ->getWorldTransform()
              .inverse()
          * annotatedGroups[i].stackedBody->bodies[j]->getWorldTransform());
    }
  }
  // 1.1.2. Compute adjacent (stacked) joints and their relative transforms
  for (int i = 0; i < mStackedJoints.size(); i++)
  {
    std::shared_ptr<struct StackedJoint> joint = mStackedJoints[i];
    Eigen::Vector3s jointCenter = getStackedJointCenterFromJointCentersVector(
        joint, jointWorldPositions);

    for (auto& annotatedGroup : annotatedGroups)
    {
      if (joint->childBody == annotatedGroup.stackedBody
          || joint->parentBody == annotatedGroup.stackedBody)
      {
        annotatedGroup.adjacentJoints.push_back(joint);
        annotatedGroup.adjacentJointCenters.push_back(
            annotatedGroup.stackedBody->bodies[0]->getWorldTransform().inverse()
            * jointCenter);
      }
    }
  }
  // 1.1.3. Compute adjacent markers and their relative transforms
  Eigen::VectorXs markerWorldPositions
      = mSkel->getMarkerWorldPositions(mMarkers);
  for (int i = 0; i < mMarkers.size(); i++)
  {
    auto& marker = mMarkers[i];
    for (auto& weldedGroup : annotatedGroups)
    {
      if (std::find(
              weldedGroup.stackedBody->bodies.begin(),
              weldedGroup.stackedBody->bodies.end(),
              marker.first)
          != weldedGroup.stackedBody->bodies.end())
      {
        weldedGroup.adjacentMarkers.push_back(mMarkerNames[i]);
        Eigen::Vector3s markerCenter = markerWorldPositions.segment<3>(i * 3);
        weldedGroup.adjacentMarkerCenters.push_back(
            weldedGroup.stackedBody->bodies[0]->getWorldTransform().inverse()
            * markerCenter);
      }
    }
  }
  const Eigen::MatrixXi& isJointParentOf = mSkel->getJointParentMap();
  Eigen::MatrixXi isStackedJointParentOfBody
      = Eigen::MatrixXi::Zero(mStackedJoints.size(), mStackedBodies.size());
  for (int i = 0; i < mStackedJoints.size(); i++)
  {
    for (int j = 0; j < mStackedBodies.size(); j++)
    {
      for (auto* potentialParent : mStackedJoints[i]->joints)
      {
        for (auto* potentialChildBody : mStackedBodies[j]->bodies)
        {
          if ((potentialParent == potentialChildBody->getParentJoint())
              || (isJointParentOf(
                      potentialParent->getJointIndexInSkeleton(),
                      potentialChildBody->getParentJoint()
                          ->getJointIndexInSkeleton())
                  > 0))
          {
            isStackedJointParentOfBody(i, j) = 1;
            break;
          }
        }
        if (isStackedJointParentOfBody(i, j))
          break;
      }
    }
  }

  // 2. Now we can go through each timestep and do "closed form IK", by first
  // estimating the body positions and then estimating the joint angles to
  // achieve those body positions.
  mPoses.clear();
  mBodyTransforms.clear();
  for (int t = 0; t < mMarkerObservations.size(); t++)
  {
    // 2.1. Estimate the body positions in world space of each welded group from
    // the joint estimates and marker estimates, when they're available.
    std::map<std::string, Eigen::Isometry3s> estimatedBodyWorldTransforms;
    for (auto& annotatedGroup : annotatedGroups)
    {
      // 2.1.1. Collect all the visible points in world space that we can use to
      // estimate the body transform
      std::vector<Eigen::Vector3s> visibleAdjacentPointsInWorldSpace;
      std::vector<std::string> visibleAdjacentPointNames;
      std::vector<Eigen::Vector3s> visibleAdjacentPointsInLocalSpace;
      std::vector<s_t> visibleAdjacentPointWeights;
      for (int j = 0; j < annotatedGroup.adjacentJoints.size(); j++)
      {
        if (mJointCenters[t].count(annotatedGroup.adjacentJoints[j]->name) > 0)
        {
          visibleAdjacentPointsInWorldSpace.push_back(
              mJointCenters[t][annotatedGroup.adjacentJoints[j]->name]);
          visibleAdjacentPointsInLocalSpace.push_back(
              annotatedGroup.adjacentJointCenters[j]);
          visibleAdjacentPointNames.push_back(
              "Joint " + annotatedGroup.adjacentJoints[j]->name);
          visibleAdjacentPointWeights.push_back(1.0);
        }
      }
      for (int j = 0; j < annotatedGroup.adjacentMarkers.size(); j++)
      {
        if (mMarkerObservations[t].count(annotatedGroup.adjacentMarkers[j]) > 0)
        {
          visibleAdjacentPointsInWorldSpace.push_back(
              mMarkerObservations[t][annotatedGroup.adjacentMarkers[j]]);
          visibleAdjacentPointsInLocalSpace.push_back(
              annotatedGroup.adjacentMarkerCenters[j]);
          visibleAdjacentPointNames.push_back(
              "Marker " + annotatedGroup.adjacentMarkers[j]);
          visibleAdjacentPointWeights.push_back(0.01);
        }
      }
      assert(
          visibleAdjacentPointsInLocalSpace.size()
          == visibleAdjacentPointsInWorldSpace.size());

      if (visibleAdjacentPointsInLocalSpace.size() >= 3)
      {
        // 3.1.2. Compute the root body transform from the visible points, and
        // then use that to compute the other body transforms
        Eigen::Isometry3s rootBodyWorldTransform
            = getPointCloudToPointCloudTransform(
                visibleAdjacentPointsInLocalSpace,
                visibleAdjacentPointsInWorldSpace,
                visibleAdjacentPointWeights);
        for (int j = 0; j < annotatedGroup.stackedBody->bodies.size(); j++)
        {
          estimatedBodyWorldTransforms[annotatedGroup.stackedBody->bodies[j]
                                           ->getName()]
              = rootBodyWorldTransform * annotatedGroup.relativeTransforms[j];
        }

        s_t error = 0.0;
        for (int j = 0; j < visibleAdjacentPointsInLocalSpace.size(); j++)
        {
          Eigen::Vector3s reconstructedPoint
              = rootBodyWorldTransform * visibleAdjacentPointsInLocalSpace[j];
          error += (visibleAdjacentPointsInWorldSpace[j] - reconstructedPoint)
                       .norm();
        }
        error /= visibleAdjacentPointsInLocalSpace.size();

        if (logOutput)
        {
          std::cout << "Estimated bodies ";
          for (int j = 0; j < annotatedGroup.stackedBody->bodies.size(); j++)
          {
            std::cout << "\""
                      << annotatedGroup.stackedBody->bodies[j]->getName()
                      << "\" ";
          }
          std::cout << " at time " << t << " with error " << error << "m from "
                    << visibleAdjacentPointsInLocalSpace.size()
                    << " points: " << std::endl;
          for (int j = 0; j < visibleAdjacentPointsInLocalSpace.size(); j++)
          {
            std::cout << "  " << visibleAdjacentPointNames[j] << ": "
                      << visibleAdjacentPointsInWorldSpace[j].transpose()
                      << " reconstructed as "
                      << (rootBodyWorldTransform
                          * visibleAdjacentPointsInLocalSpace[j])
                             .transpose()
                      << std::endl;
          }
        }
      }
    }
    mBodyTransforms.push_back(estimatedBodyWorldTransforms);

    // 2.2. Now that we have the estimated body positions, we can estimate the
    // joint angles.
    Eigen::VectorXs jointAngles = Eigen::VectorXs::Zero(mSkel->getNumDofs());
    Eigen::VectorXi jointAnglesClosedFormEstimate
        = Eigen::VectorXi::Zero(mSkel->getNumDofs());

    int NUM_PASSES = 1;
    for (int pass = 0; pass < NUM_PASSES; pass++)
    {
      std::map<std::string, bool> solvedJoints;
      std::map<std::string, Eigen::Isometry3s> jointChildTransform;
      if (logOutput)
      {
        std::cout << "Running IK Pass " << pass << "/" << NUM_PASSES
                  << " upward" << std::endl;
      }
      while (true)
      {
        bool foundJointToSolve = false;
        // We want to accumulate snowballs of joints as we roll up the kinematic
        // tree, from the leaves inwards. We want to solve the leaves using just
        // the world transform we already have computed, but then for higher
        // level joints we want to also include the solved children in the point
        // cloud fit.
        for (int i = 0; i < mStackedJoints.size(); i++)
        {
          auto& joint = mStackedJoints[i];
          if (solvedJoints.count(joint->name))
            continue;
          // Only estimate joint angles for joints connecting two bodies that we
          // have estimated locations for
          if (!(joint->parentBody == nullptr
                || estimatedBodyWorldTransforms.count(joint->parentBody->name))
              || estimatedBodyWorldTransforms.count(joint->childBody->name)
                     == 0)
            continue;

          bool solvedAllChildren = true;
          for (int i = 0; i < joint->childBody->childJoints.size(); i++)
          {
            if (solvedJoints.count(joint->childBody->childJoints[i]->name) == 0)
            {
              solvedAllChildren = false;
              break;
            }
          }

          if (!solvedAllChildren)
            continue;

          // 2.2.1. Get the relative transformation we estimate is taking place
          // over the joint
          Eigen::Isometry3s parentTransform = Eigen::Isometry3s::Identity();
          if (joint->parentBody != nullptr)
          {
            parentTransform
                = estimatedBodyWorldTransforms[joint->parentBody->name];
          }
          Eigen::Isometry3s childTransform
              = estimatedBodyWorldTransforms[joint->childBody->name];

          if (joint->childBody->childJoints.size() == 0)
          {
            // 2.2.1. If this is a leaf node, read the transform off of the body
            // transforms already computed
          }
          else
          {
            // 2.2.2. If we've got children, we want to include their joint
            // angles in the point cloud fit. This is somewhat more involved.
            // First step is to set the skeleton into the position we've
            // estimated so far.
            mSkel->setPositions(jointAngles);
            Eigen::VectorXs jointWorldPositions
                = mSkel->getJointWorldPositions(mSkel->getJoints());
            Eigen::VectorXs markerWorldPositions
                = mSkel->getMarkerWorldPositions(mMarkers);
            Eigen::Isometry3s rootBodyWorldTransform
                = joint->childBody->bodies[0]->getWorldTransform();

            // 2.2.3. Now we need to go through the children and get a point
            // cloud in the coordinates of the root child body
            std::vector<Eigen::Vector3s> visibleAdjacentPointsInWorldSpace;
            std::vector<Eigen::Vector3s> visibleAdjacentPointsInLocalSpace;
            std::vector<s_t> visibleAdjacentPointWeights;

            for (int j = 0; j < annotatedGroups.size(); j++)
            {
              if (isStackedJointParentOfBody(i, j))
              {
                auto& annotatedGroup = annotatedGroups[j];
                for (int j = 0; j < annotatedGroup.adjacentJoints.size(); j++)
                {
                  if (mJointCenters[t].count(
                          annotatedGroup.adjacentJoints[j]->name)
                      > 0)
                  {
                    visibleAdjacentPointsInWorldSpace.push_back(
                        mJointCenters[t]
                                     [annotatedGroup.adjacentJoints[j]->name]);
                    visibleAdjacentPointsInLocalSpace.push_back(
                        rootBodyWorldTransform.inverse()
                        * getStackedJointCenterFromJointCentersVector(
                            annotatedGroup.adjacentJoints[j],
                            jointWorldPositions));
                    visibleAdjacentPointWeights.push_back(1.0);
                  }
                }
                for (int j = 0; j < annotatedGroup.adjacentMarkers.size(); j++)
                {
                  if (mMarkerObservations[t].count(
                          annotatedGroup.adjacentMarkers[j])
                      > 0)
                  {
                    visibleAdjacentPointsInWorldSpace.push_back(
                        mMarkerObservations[t]
                                           [annotatedGroup.adjacentMarkers[j]]);
                    int markerIndex = std::find(
                                          mMarkerNames.begin(),
                                          mMarkerNames.end(),
                                          annotatedGroup.adjacentMarkers[j])
                                      - mMarkerNames.begin();
                    visibleAdjacentPointsInLocalSpace.push_back(
                        rootBodyWorldTransform.inverse()
                        * markerWorldPositions.segment<3>(markerIndex * 3));
                    visibleAdjacentPointWeights.push_back(0.01);
                  }
                }
              }
            }
            assert(
                visibleAdjacentPointsInLocalSpace.size()
                == visibleAdjacentPointsInWorldSpace.size());

            // 3.1.2. Compute the root body transform from the visible points,
            // and then use that to compute the other body transforms
            childTransform = getPointCloudToPointCloudTransform(
                visibleAdjacentPointsInLocalSpace,
                visibleAdjacentPointsInWorldSpace,
                visibleAdjacentPointWeights);

            if (NUM_PASSES > 1)
            {
              // Update the estimated body transform for next iteration
              estimatedBodyWorldTransforms[joint->childBody->name]
                  = childTransform;
            }
          }
          jointChildTransform[joint->name] = childTransform;

          Eigen::Isometry3s totalJointTransform
              = parentTransform.inverse() * childTransform;

          Eigen::Isometry3s remainingJointTransform = totalJointTransform;
          for (int j = 0; j < joint->joints.size(); j++)
          {
            dynamics::Joint* subJoint = joint->joints[j];

            // 2.2.2. Convert the remaining estimated tranformation into joint
            // coordinates on this joint
            Eigen::VectorXs pos = subJoint->getNearestPositionToDesiredRotation(
                remainingJointTransform.linear());
            subJoint->setPositions(pos);
            Eigen::Isometry3s recoveredJointTransform
                = subJoint->getRelativeTransform();
            if (subJoint->getType() == dynamics::FreeJoint::getStaticType()
                || subJoint->getType()
                       == dynamics::EulerFreeJoint::getStaticType())
            {
              Eigen::Vector3s translationOffset
                  = (recoveredJointTransform.translation()
                     - totalJointTransform.translation());
              pos.tail<3>() -= translationOffset;
              subJoint->setPositions(pos);
              recoveredJointTransform = subJoint->getRelativeTransform();
            }
            remainingJointTransform
                = recoveredJointTransform.inverse() * remainingJointTransform;

            if (logOutput)
            {
              s_t translationError = (totalJointTransform.translation()
                                      - recoveredJointTransform.translation())
                                         .norm();
              s_t rotationError = (totalJointTransform.linear()
                                   - recoveredJointTransform.linear())
                                      .norm();
              std::cout << "Estimating joint " << subJoint->getName()
                        << " from adjacent body transforms with error "
                        << translationError << "m and " << rotationError
                        << " on rotation" << std::endl;
            }

            // 2.2.3. Save our estimate back to our joint angles
            jointAngles.segment(
                subJoint->getIndexInSkeleton(0), subJoint->getNumDofs())
                = pos;
            jointAnglesClosedFormEstimate
                .segment(
                    subJoint->getIndexInSkeleton(0), subJoint->getNumDofs())
                .setConstant(1.0);
          }

          solvedJoints[joint->name] = true;
          foundJointToSolve = true;
        }

        if (!foundJointToSolve)
          break;
      }

      // Now we want to do one more pass, this time going from the root back out
      // to the leaves, where we can re-estimate the joint angles now that the
      // parent body may have changed its location.

      if (logOutput)
      {
        std::cout << "Running IK Pass " << pass << "/" << NUM_PASSES
                  << " downward" << std::endl;
      }
      std::map<std::string, bool> solvedParents;
      while (true)
      {
        bool foundJointToSolve = false;
        // We want to accumulate snowballs of joints as we roll up the kinematic
        // tree, from the leaves inwards. We want to solve the leaves using just
        // the world transform we already have computed, but then for higher
        // level joints we want to also include the solved children in the point
        // cloud fit.
        for (int i = 0; i < mStackedJoints.size(); i++)
        {
          // No need to solve twice
          if (solvedParents.count(mStackedJoints[i]->name))
            continue;

          // If we haven't solved the parent joint yet, continue
          if (mStackedJoints[i]->parentBody != nullptr
              && solvedParents.count(
                     mStackedJoints[i]->parentBody->parentJoint->name)
                     == 0)
            continue;

          std::shared_ptr<struct StackedJoint> joint = mStackedJoints[i];

          Eigen::Isometry3s parentTransform = Eigen::Isometry3s::Identity();
          if (joint->parentBody != nullptr)
          {
            mSkel->setPositions(jointAngles);
            parentTransform = joint->parentBody->bodies[0]->getWorldTransform();
            if (NUM_PASSES > 1)
            {
              // Update the estimated body transform for next iteration
              estimatedBodyWorldTransforms[joint->parentBody->name]
                  = parentTransform;
            }
          }
          Eigen::Isometry3s childTransform = jointChildTransform[joint->name];

          Eigen::Isometry3s totalJointTransform
              = parentTransform.inverse() * childTransform;

          Eigen::Isometry3s remainingJointTransform = totalJointTransform;
          for (int j = 0; j < joint->joints.size(); j++)
          {
            dynamics::Joint* subJoint = joint->joints[j];

            // 2.2.2. Convert the remaining estimated tranformation into joint
            // coordinates on this joint
            Eigen::VectorXs pos = subJoint->getNearestPositionToDesiredRotation(
                remainingJointTransform.linear());
            subJoint->setPositions(pos);
            Eigen::Isometry3s recoveredJointTransform
                = subJoint->getRelativeTransform();
            if (subJoint->getType() == dynamics::FreeJoint::getStaticType()
                || subJoint->getType()
                       == dynamics::EulerFreeJoint::getStaticType())
            {
              Eigen::Vector3s translationOffset
                  = (recoveredJointTransform.translation()
                     - totalJointTransform.translation());
              pos.tail<3>() -= translationOffset;
              subJoint->setPositions(pos);
              recoveredJointTransform = subJoint->getRelativeTransform();
            }
            remainingJointTransform
                = recoveredJointTransform.inverse() * remainingJointTransform;

            if (logOutput)
            {
              s_t translationError = (totalJointTransform.translation()
                                      - recoveredJointTransform.translation())
                                         .norm();
              s_t rotationError = (totalJointTransform.linear()
                                   - recoveredJointTransform.linear())
                                      .norm();
              std::cout
                  << "Re-estimating (this time with a known parent) joint "
                  << subJoint->getName()
                  << " from adjacent body transforms with error "
                  << translationError << "m and " << rotationError
                  << " on rotation" << std::endl;
            }

            // 2.2.3. Save our estimate back to our joint angles
            jointAngles.segment(
                subJoint->getIndexInSkeleton(0), subJoint->getNumDofs())
                = pos;
            jointAnglesClosedFormEstimate
                .segment(
                    subJoint->getIndexInSkeleton(0), subJoint->getNumDofs())
                .setConstant(1.0);
          }

          solvedParents[mStackedJoints[i]->name] = true;
          foundJointToSolve = true;
          break;
        }
        if (!foundJointToSolve)
          break;
      }
    }

    mPoses.push_back(jointAngles);
    mPosesClosedFormEstimateAvailable.push_back(jointAnglesClosedFormEstimate);
  }

  mSkel->setPositions(originalPose);

  // TODO: compute marker reconstruction error and return that
  return 0.0;
}

/// This solves the remaining DOFs that couldn't be found in closed form using
/// an iterative IK solver. This portion of the solver is the only non-convex
/// portion. It uses random-restarts, and so is not as unit-testable as the
/// other portions of the algorithm, so it should hopefully only impact less
/// important joints.
s_t IKInitializer::completeIKIteratively(
    int timestep, std::shared_ptr<dynamics::Skeleton> threadsafeSkel)
{
  Eigen::VectorXs analyticalPose = mPoses[timestep];
  Eigen::VectorXi analyticalPoseClosedFormEstimate
      = mPosesClosedFormEstimateAvailable[timestep];

  int numClosedForm = 0;
  for (int i = 0; i < analyticalPoseClosedFormEstimate.size(); i++)
  {
    if (analyticalPoseClosedFormEstimate(i) == 1)
    {
      numClosedForm++;
    }
  }
  int numDofsToSolve = analyticalPose.size() - numClosedForm;
  if (numDofsToSolve == 0)
    return 0.0;

  Eigen::VectorXs initialGuess = Eigen::VectorXs::Zero(numDofsToSolve);
  Eigen::VectorXs upperLimit = Eigen::VectorXs::Zero(numDofsToSolve);
  Eigen::VectorXs lowerLimit = Eigen::VectorXs::Zero(numDofsToSolve);
  int cursor = 0;
  for (int i = 0; i < analyticalPose.size(); i++)
  {
    if (analyticalPoseClosedFormEstimate(i) == 0)
    {
      upperLimit(cursor) = threadsafeSkel->getDof(i)->getPositionUpperLimit();
      lowerLimit(cursor) = threadsafeSkel->getDof(i)->getPositionLowerLimit();
      cursor++;
    }
  }

  std::vector<std::pair<dynamics::BodyNode*, Eigen::Vector3s>> markers
      = getVisibleMarkers(timestep);
  std::vector<std::string> markerNames = getVisibleMarkerNames(timestep);
  std::vector<std::shared_ptr<struct StackedJoint>> otherSkelJoints
      = getVisibleJoints(timestep);
  std::vector<dynamics::Joint*> joints;
  for (auto& j : otherSkelJoints)
  {
    // TODO: merge joints together
    if (j->joints.size() == 1)
    {
      for (auto* joint : j->joints)
      {
        joints.push_back(threadsafeSkel->getJoint(joint->getName()));
      }
    }
  }

  Eigen::VectorXs goal
      = Eigen::VectorXs::Zero(markers.size() * 3 + joints.size() * 3);
  for (int i = 0; i < markerNames.size(); i++)
  {
    goal.segment<3>(i * 3) = mMarkerObservations[timestep][markerNames[i]];
  }
  for (int i = 0; i < joints.size(); i++)
  {
    goal.segment<3>(markerNames.size() * 3 + i * 3)
        = mJointCenters[timestep][joints[i]->getName()];
  }

  math::solveIK(
      initialGuess,
      upperLimit,
      lowerLimit,
      goal.size(),
      // Set positions
      [threadsafeSkel, analyticalPose, analyticalPoseClosedFormEstimate](
          /* in*/ const Eigen::VectorXs pos, bool clamp) {
        (void)clamp;
        // Complete the root position to the rest of the skeleton
        Eigen::VectorXs fullPos = analyticalPose;
        int cursor = 0;
        for (int i = 0; i < analyticalPoseClosedFormEstimate.size(); i++)
        {
          if (analyticalPoseClosedFormEstimate(i) == 0)
          {
            fullPos(i) = pos(cursor);
            cursor++;
          }
        }
        threadsafeSkel->setPositions(fullPos);

        // Return the root position
        return pos;
      },
      // Compute the Jacobian
      [threadsafeSkel,
       analyticalPose,
       analyticalPoseClosedFormEstimate,
       goal,
       markers,
       joints,
       numDofsToSolve](
          /*out*/ Eigen::Ref<Eigen::VectorXs> diff,
          /*out*/ Eigen::Ref<Eigen::MatrixXs> jac) {
        diff.segment(0, markers.size() * 3)
            = threadsafeSkel->getMarkerWorldPositions(markers)
              - goal.segment(0, markers.size() * 3);
        diff.segment(markers.size() * 3, joints.size() * 3)
            = threadsafeSkel->getJointWorldPositions(joints)
              - goal.segment(markers.size() * 3, joints.size() * 3);

        (void)numDofsToSolve;
        assert(jac.cols() == numDofsToSolve);
        assert(jac.rows() == goal.size());
        jac.setZero();

        Eigen::MatrixXs markerJac
            = threadsafeSkel->getMarkerWorldPositionsJacobianWrtJointPositions(
                markers);
        Eigen::MatrixXs jointJac
            = threadsafeSkel->getJointWorldPositionsJacobianWrtJointPositions(
                joints);

        int cursor = 0;
        for (int i = 0; i < analyticalPoseClosedFormEstimate.size(); i++)
        {
          if (analyticalPoseClosedFormEstimate(i) == 0)
          {
            jac.block(0, cursor, markerJac.rows(), 1)
                = markerJac.block(0, i, markerJac.rows(), 1);
            jac.block(markerJac.rows(), cursor, jointJac.rows(), 1)
                = jointJac.block(0, i, jointJac.rows(), 1);
            cursor++;
          }
        }
      },
      // Generate a random restart position
      [threadsafeSkel, analyticalPoseClosedFormEstimate, numDofsToSolve](
          Eigen::Ref<Eigen::VectorXs> val) {
        Eigen::VectorXs fullRandom = threadsafeSkel->getRandomPose();

        Eigen::VectorXs random = Eigen::VectorXs::Zero(numDofsToSolve);
        int cursor = 0;
        for (int i = 0; i < analyticalPoseClosedFormEstimate.size(); i++)
        {
          if (analyticalPoseClosedFormEstimate(i) == 0)
          {
            random(cursor) = fullRandom(i);
            cursor++;
          }
        }
        val = random;
      },
      math::IKConfig()
          .setMaxStepCount(150)
          .setConvergenceThreshold(1e-10)
          .setMaxRestarts(3)
          .setLogOutput(false));

  mPoses[timestep] = threadsafeSkel->getPositions();
  return 0.0;
}

/// This solves ALL the DOFs, including the ones that were found in closed
/// form, to fine tune loss.
s_t IKInitializer::fineTuneIKIteratively(
    int timestep, std::shared_ptr<dynamics::Skeleton> threadsafeSkel)
{
  Eigen::VectorXs analyticalPose = mPoses[timestep];

  Eigen::VectorXs initialGuess
      = Eigen::VectorXs::Zero(threadsafeSkel->getNumDofs());
  Eigen::VectorXs upperLimit
      = Eigen::VectorXs::Zero(threadsafeSkel->getNumDofs());
  Eigen::VectorXs lowerLimit
      = Eigen::VectorXs::Zero(threadsafeSkel->getNumDofs());

  std::vector<std::pair<dynamics::BodyNode*, Eigen::Vector3s>> markers
      = getVisibleMarkers(timestep);
  std::vector<std::string> markerNames = getVisibleMarkerNames(timestep);
  std::vector<std::shared_ptr<struct StackedJoint>> otherSkelJoints
      = getVisibleJoints(timestep);
  std::vector<dynamics::Joint*> joints;
  for (auto& j : otherSkelJoints)
  {
    if (j->joints.size() == 1)
    {
      for (auto* joint : j->joints)
      {
        joints.push_back(threadsafeSkel->getJoint(joint->getName()));
      }
    }
  }

  Eigen::VectorXs goal
      = Eigen::VectorXs::Zero(markers.size() * 3 + joints.size() * 3);
  for (int i = 0; i < markerNames.size(); i++)
  {
    goal.segment<3>(i * 3) = mMarkerObservations[timestep][markerNames[i]];
  }
  for (int i = 0; i < joints.size(); i++)
  {
    goal.segment<3>(markerNames.size() * 3 + i * 3)
        = mJointCenters[timestep][joints[i]->getName()];
  }

  math::solveIK(
      initialGuess,
      threadsafeSkel->getPositionUpperLimits(),
      threadsafeSkel->getPositionLowerLimits(),
      goal.size(),
      // Set positions
      [threadsafeSkel, analyticalPose](
          /* in*/ const Eigen::VectorXs pos, bool clamp) {
        Eigen::VectorXs clampedPos = pos;
        if (clamp)
        {
          clampedPos
              = clampedPos.cwiseMax(threadsafeSkel->getPositionLowerLimits());
          clampedPos
              = clampedPos.cwiseMin(threadsafeSkel->getPositionUpperLimits());
        }
        // Complete the root position to the rest of the skeleton
        threadsafeSkel->setPositions(clampedPos);
        // Return the root position
        return pos;
      },
      // Compute the Jacobian
      [threadsafeSkel, analyticalPose, goal, markers, joints](
          /*out*/ Eigen::Ref<Eigen::VectorXs> diff,
          /*out*/ Eigen::Ref<Eigen::MatrixXs> jac) {
        diff.segment(0, markers.size() * 3)
            = threadsafeSkel->getMarkerWorldPositions(markers)
              - goal.segment(0, markers.size() * 3);
        diff.segment(markers.size() * 3, joints.size() * 3)
            = threadsafeSkel->getJointWorldPositions(joints)
              - goal.segment(markers.size() * 3, joints.size() * 3);

        assert(jac.cols() == threadsafeSkel->getNumDofs());
        assert(jac.rows() == goal.size());
        jac.setZero();

        Eigen::MatrixXs markerJac
            = threadsafeSkel->getMarkerWorldPositionsJacobianWrtJointPositions(
                markers);
        Eigen::MatrixXs jointJac
            = threadsafeSkel->getJointWorldPositionsJacobianWrtJointPositions(
                joints);
        jac.block(0, 0, markerJac.rows(), markerJac.cols()) = markerJac;
        jac.block(markerJac.rows(), 0, jointJac.rows(), jointJac.cols())
            = jointJac;
      },
      // Generate a random restart position
      [threadsafeSkel](Eigen::Ref<Eigen::VectorXs> val) {
        Eigen::VectorXs fullRandom = threadsafeSkel->getRandomPose();
        val = fullRandom;
      },
      math::IKConfig()
          .setMaxStepCount(150)
          .setConvergenceThreshold(1e-10)
          .setMaxRestarts(1)
          .setLogOutput(false));

  mPoses[timestep] = threadsafeSkel->getPositions();
  return 0.0;
}

/// This uses the current guesses for the joint centers to re-estimate the
/// distances between the markers and the joint centers, and the distances
/// between the adjacent joints.
void IKInitializer::reestimateDistancesFromJointCenters()
{
  // 1. Recompute the joint to joint squared distances

  std::map<std::string, std::map<std::string, s_t>>
      jointToJointSquaredDistances;
  std::map<std::string, std::map<std::string, int>>
      jointToJointSquaredDistancesCount;
  for (int t = 0; t < mMarkerObservations.size(); t++)
  {
    for (auto& pair : mJointToJointSquaredDistances)
    {
      std::string joint1Name = pair.first;
      for (auto& pair2 : pair.second)
      {
        std::string joint2Name = pair2.first;
        if (joint1Name == joint2Name)
        {
          continue;
        }
        if (mJointCenters[t].count(joint1Name) == 0)
        {
          continue;
        }
        if (mJointCenters[t].count(joint2Name) == 0)
        {
          continue;
        }
        Eigen::Vector3s joint1Center = mJointCenters[t][joint1Name];
        Eigen::Vector3s joint2Center = mJointCenters[t][joint2Name];
        s_t squaredDistance = (joint1Center - joint2Center).squaredNorm();
        if (jointToJointSquaredDistances[joint1Name].count(joint2Name) == 0)
        {
          jointToJointSquaredDistances[joint1Name][joint2Name] = 0;
          jointToJointSquaredDistancesCount[joint1Name][joint2Name] = 0;
        }
        jointToJointSquaredDistances[joint1Name][joint2Name] += squaredDistance;
        jointToJointSquaredDistancesCount[joint1Name][joint2Name]++;
      }
    }
  }
  for (auto& pair : jointToJointSquaredDistances)
  {
    std::string joint1Name = pair.first;
    for (auto& pair2 : pair.second)
    {
      std::string joint2Name = pair2.first;
      s_t squaredDistance
          = pair2.second
            / jointToJointSquaredDistancesCount[joint1Name][joint2Name];
      mJointToJointSquaredDistances[joint1Name][joint2Name] = squaredDistance;
      mJointToJointSquaredDistances[joint2Name][joint1Name] = squaredDistance;
    }
  }

  // 2. Do the same thing, but for joint to marker distances

  std::map<std::string, std::map<std::string, s_t>>
      jointToMarkerSquaredDistances;
  std::map<std::string, std::map<std::string, int>>
      jointToMarkerSquaredDistancesCount;
  for (int t = 0; t < mMarkerObservations.size(); t++)
  {
    for (auto& pair : mJointToMarkerSquaredDistances)
    {
      std::string jointName = pair.first;
      for (auto& pair2 : pair.second)
      {
        std::string markerName = pair2.first;
        if (mJointCenters[t].count(jointName) == 0)
        {
          continue;
        }
        if (mMarkerObservations[t].count(markerName) == 0)
        {
          continue;
        }
        Eigen::Vector3s jointCenter = mJointCenters[t][jointName];
        Eigen::Vector3s marker = mMarkerObservations[t][markerName];
        s_t squaredDistance = (jointCenter - marker).squaredNorm();
        if (jointToMarkerSquaredDistances[jointName].count(markerName) == 0)
        {
          jointToMarkerSquaredDistances[jointName][markerName] = 0;
          jointToMarkerSquaredDistancesCount[jointName][markerName] = 0;
        }
        jointToMarkerSquaredDistances[jointName][markerName] += squaredDistance;
        jointToMarkerSquaredDistancesCount[jointName][markerName]++;
      }
    }
  }
  for (auto& pair : jointToMarkerSquaredDistances)
  {
    std::string jointName = pair.first;
    for (auto& pair2 : pair.second)
    {
      std::string markerName = pair2.first;
      s_t squaredDistance
          = pair2.second
            / jointToMarkerSquaredDistancesCount[jointName][markerName];
      mJointToMarkerSquaredDistances[jointName][markerName] = squaredDistance;
      mJointToMarkerSquaredDistances[jointName][markerName] = squaredDistance;
    }
  }
}

//==============================================================================
/// This gets the average distance between adjacent joint centers in our
/// current joint center estimates.
std::map<std::string, std::map<std::string, s_t>>
IKInitializer::estimateJointToJointDistances()
{
  // 1. Recompute the joint to joint squared distances

  std::map<std::string, std::map<std::string, s_t>> jointToJointAvgDistances;
  std::map<std::string, std::map<std::string, int>>
      jointToJointAvgDistancesCount;
  for (int t = 0; t < mMarkerObservations.size(); t++)
  {
    for (auto& pair : mJointToJointSquaredDistances)
    {
      std::string joint1Name = pair.first;
      for (auto& pair2 : pair.second)
      {
        std::string joint2Name = pair2.first;
        if (joint1Name == joint2Name)
        {
          continue;
        }
        if (mJointCenters[t].count(joint1Name) == 0)
        {
          continue;
        }
        if (mJointCenters[t].count(joint2Name) == 0)
        {
          continue;
        }
        Eigen::Vector3s joint1Center = mJointCenters[t][joint1Name];
        Eigen::Vector3s joint2Center = mJointCenters[t][joint2Name];
        s_t distance = (joint1Center - joint2Center).norm();
        if (jointToJointAvgDistances[joint1Name].count(joint2Name) == 0)
        {
          jointToJointAvgDistances[joint1Name][joint2Name] = 0;
          jointToJointAvgDistancesCount[joint1Name][joint2Name] = 0;
        }
        jointToJointAvgDistances[joint1Name][joint2Name] += distance;
        jointToJointAvgDistancesCount[joint1Name][joint2Name]++;
      }
    }
  }
  for (auto& pair : jointToJointAvgDistances)
  {
    std::string joint1Name = pair.first;
    for (auto& pair2 : pair.second)
    {
      std::string joint2Name = pair2.first;
      jointToJointAvgDistances[joint1Name][joint2Name]
          /= jointToJointAvgDistancesCount[joint1Name][joint2Name];
    }
  }
  return jointToJointAvgDistances;
}

//==============================================================================
/// This gets the subset of markers that are visible at a given timestep
std::vector<std::pair<dynamics::BodyNode*, Eigen::Vector3s>>
IKInitializer::getVisibleMarkers(int t)
{
  std::vector<std::pair<dynamics::BodyNode*, Eigen::Vector3s>> markers;
  for (int i = 0; i < mMarkerNames.size(); i++)
  {
    if (mMarkerObservations[t].count(mMarkerNames[i]))
    {
      markers.push_back(mMarkers[i]);
    }
  }
  return markers;
}

//==============================================================================
/// This gets the subset of markers that are visible at a given timestep
std::vector<std::string> IKInitializer::getVisibleMarkerNames(int t)
{
  std::vector<std::string> markerNames;
  for (int i = 0; i < mMarkerNames.size(); i++)
  {
    if (mMarkerObservations[t].count(mMarkerNames[i]))
    {
      markerNames.push_back(mMarkerNames[i]);
    }
  }
  return markerNames;
}

//==============================================================================
/// This gets the subset of joints that are attached to markers that are
/// visible at a given timestep
std::vector<std::shared_ptr<struct StackedJoint>>
IKInitializer::getVisibleJoints(int t)
{
  std::vector<std::shared_ptr<struct StackedJoint>> joints;
  for (int i = 0; i < mStackedJoints.size(); i++)
  {
    std::shared_ptr<struct StackedJoint> joint = mStackedJoints[i];
    if (mJointToMarkerSquaredDistances.count(joint->name) == 0)
    {
      continue;
    }
    for (auto& pair : mJointToMarkerSquaredDistances[joint->name])
    {
      if (mMarkerObservations[t].count(pair.first))
      {
        joints.push_back(joint);
        break;
      }
    }
  }
  return joints;
}

//==============================================================================
/// This gets the world center estimates for joints that are attached to
/// markers that are visible at this timestep.
std::map<std::string, Eigen::Vector3s> IKInitializer::getVisibleJointCenters(
    int t)
{
  return mJointCenters[t];
}

//==============================================================================
/// This gets the squared distance between a joint and a marker on an adjacent
/// body segment.
std::map<std::string, s_t> IKInitializer::getJointToMarkerSquaredDistances(
    std::string jointName)
{
  return mJointToMarkerSquaredDistances[jointName];
}

//==============================================================================
/// This does a rank-N completion of the distance matrix, which provides some
/// guarantees about reconstruction quality. See "Distance Matrix
/// Reconstruction from Incomplete Distance Information for Sensor Network
/// Localization"
Eigen::MatrixXs IKInitializer::rankNDistanceMatrix(
    const Eigen::MatrixXs& distances, int n)
{
  Eigen::JacobiSVD<Eigen::MatrixXs> svd(
      distances, Eigen::ComputeThinU | Eigen::ComputeThinV);
  Eigen::MatrixXs U = svd.matrixU();
  Eigen::VectorXs S = svd.singularValues();
  Eigen::MatrixXs V = svd.matrixV();

  Eigen::MatrixXs Ur = U.leftCols(n);
  Eigen::MatrixXs Vr = V.leftCols(n);
  Eigen::VectorXs Sr = S.head(n);

  Eigen::MatrixXs rank_five_approximation
      = Ur * Sr.asDiagonal() * Vr.transpose();
  return rank_five_approximation;
}

//==============================================================================
/// This will reconstruct a centered Euclidean point cloud from a distance
/// matrix.
Eigen::MatrixXs IKInitializer::getPointCloudFromDistanceMatrix(
    const Eigen::MatrixXs& distances)
{
  int n = distances.rows();
  Eigen::MatrixXs centering_matrix = Eigen::MatrixXs::Identity(n, n)
                                     - Eigen::MatrixXs::Constant(n, n, 1.0 / n);
  Eigen::MatrixXs double_centering_matrix
      = -0.5 * centering_matrix * distances * centering_matrix;

  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXs> eigensolver(
      double_centering_matrix);
  Eigen::VectorXs eigenvalues = eigensolver.eigenvalues();
  Eigen::MatrixXs eigenvectors = eigensolver.eigenvectors();
  assert(eigensolver.info() == Eigen::Success);
  assert(!eigenvalues.hasNaN());
  assert(!eigenvectors.hasNaN());

  int k = 3;
  Eigen::MatrixXs k_eigenvectors = eigenvectors.rightCols(k);
  Eigen::VectorXs k_eigenvalues
      = eigenvalues.tail(k).cwiseMax(1e-16).cwiseSqrt();
  assert(!k_eigenvalues.hasNaN());
  assert(!k_eigenvectors.hasNaN());

  return k_eigenvalues.asDiagonal() * k_eigenvectors.transpose();
}

//==============================================================================
/// This will rotate and translate a point cloud to match the first N points
/// as closely as possible to the passed in matrix
Eigen::MatrixXs IKInitializer::mapPointCloudToData(
    const Eigen::MatrixXs& pointCloud,
    std::vector<Eigen::Vector3s> firstNPoints)
{
  Eigen::Matrix<s_t, 3, Eigen::Dynamic> targetPointCloud
      = Eigen::Matrix<s_t, 3, Eigen::Dynamic>::Zero(3, firstNPoints.size());
  for (int i = 0; i < firstNPoints.size(); i++)
  {
    targetPointCloud.col(i) = firstNPoints[i];
  }
  Eigen::Matrix<s_t, 3, Eigen::Dynamic> sourcePointCloud
      = pointCloud.block(0, 0, 3, firstNPoints.size());

  assert(sourcePointCloud.cols() == targetPointCloud.cols());

  // Compute the centroids of the source and target points
  Eigen::Vector3s sourceCentroid = sourcePointCloud.rowwise().mean();
  Eigen::Vector3s targetCentroid = targetPointCloud.rowwise().mean();

#ifndef NDEBUG
  Eigen::Vector3s sourceAvg = Eigen::Vector3s::Zero();
  Eigen::Vector3s targetAvg = Eigen::Vector3s::Zero();
  for (int i = 0; i < sourcePointCloud.cols(); i++)
  {
    sourceAvg += sourcePointCloud.col(i);
    targetAvg += targetPointCloud.col(i);
  }
  sourceAvg /= sourcePointCloud.cols();
  targetAvg /= targetPointCloud.cols();
  assert((sourceAvg - sourceCentroid).norm() < 1e-12);
  assert((targetAvg - targetCentroid).norm() < 1e-12);
#endif

  // Compute the centered source and target points
  Eigen::Matrix<s_t, 3, Eigen::Dynamic> centeredSourcePoints
      = sourcePointCloud.colwise() - sourceCentroid;
  Eigen::Matrix<s_t, 3, Eigen::Dynamic> centeredTargetPoints
      = targetPointCloud.colwise() - targetCentroid;

#ifndef NDEBUG
  assert(std::abs(centeredSourcePoints.rowwise().mean().norm()) < 1e-8);
  assert(std::abs(centeredTargetPoints.rowwise().mean().norm()) < 1e-8);
  for (int i = 0; i < sourcePointCloud.cols(); i++)
  {
    Eigen::Vector3s expectedCenteredSourcePoints
        = sourcePointCloud.col(i) - sourceAvg;
    assert(
        (centeredSourcePoints.col(i) - expectedCenteredSourcePoints).norm()
        < 1e-12);
    Eigen::Vector3s expectedCenteredTargetPoints
        = targetPointCloud.col(i) - targetAvg;
    assert(
        (centeredTargetPoints.col(i) - expectedCenteredTargetPoints).norm()
        < 1e-12);
  }
#endif

  // Compute the covariance matrix
  Eigen::Matrix3s covarianceMatrix
      = centeredTargetPoints * centeredSourcePoints.transpose();

  // Compute the singular value decomposition of the covariance matrix
  Eigen::JacobiSVD<Eigen::Matrix3s> svd(
      covarianceMatrix, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Eigen::Matrix3s U = svd.matrixU();
  Eigen::Matrix3s V = svd.matrixV();

  // Compute the rotation matrix and translation vector
  Eigen::Matrix3s R = U * V.transpose();
  // Normally, we would want to check the determinant of R here, to ensure that
  // we're only doing right-handed rotations. HOWEVER, because we're using a
  // point cloud, we may actually have to flip the data along an axis to get it
  // to match up, so we skip the determinant check.

  // Transform the source point cloud to the target point cloud
  Eigen::MatrixXs transformed = Eigen::MatrixXs::Zero(3, pointCloud.cols());
  for (int i = 0; i < pointCloud.cols(); i++)
  {
    transformed.col(i)
        = R * (pointCloud.col(i).head<3>() - sourceCentroid) + targetCentroid;
  }
  return transformed;
}

//==============================================================================
/// This will give the world transform necessary to apply to the local points
/// (worldT * p[i] for all localPoints) to get the local points to match the
/// world points as closely as possible.
Eigen::Isometry3s IKInitializer::getPointCloudToPointCloudTransform(
    std::vector<Eigen::Vector3s> localPoints,
    std::vector<Eigen::Vector3s> worldPoints,
    std::vector<s_t> weights)
{
  assert(localPoints.size() > 0);
  assert(worldPoints.size() > 0);
  assert(localPoints.size() == worldPoints.size());

  // Compute the centroids of the local and world points
  Eigen::Vector3s localCentroid = Eigen::Vector3s::Zero();
  s_t sumWeights = 0.0;
  for (int i = 0; i < localPoints.size(); i++)
  {
    Eigen::Vector3s& point = localPoints[i];
    sumWeights += weights[i];
    localCentroid += point * weights[i];
  }
  localCentroid /= sumWeights;
  Eigen::Vector3s worldCentroid = Eigen::Vector3s::Zero();
  for (int i = 0; i < worldPoints.size(); i++)
  {
    Eigen::Vector3s& point = worldPoints[i];
    worldCentroid += point * weights[i];
  }
  worldCentroid /= sumWeights;

  // Compute the centered local and world points
  std::vector<Eigen::Vector3s> centeredLocalPoints;
  std::vector<Eigen::Vector3s> centeredWorldPoints;
  for (int i = 0; i < localPoints.size(); i++)
  {
    centeredLocalPoints.push_back(localPoints[i] - localCentroid);
    centeredWorldPoints.push_back(worldPoints[i] - worldCentroid);
  }

  // Compute the covariance matrix
  Eigen::Matrix3s covarianceMatrix = Eigen::Matrix3s::Zero();
  for (int i = 0; i < localPoints.size(); i++)
  {
    covarianceMatrix += weights[i] * centeredWorldPoints[i]
                        * centeredLocalPoints[i].transpose();
  }

  // Compute the singular value decomposition of the covariance matrix
  Eigen::JacobiSVD<Eigen::Matrix3s> svd(
      covarianceMatrix, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Eigen::Matrix3s U = svd.matrixU();
  Eigen::Matrix3s V = svd.matrixV();

  // Compute the rotation matrix and translation vector
  Eigen::Matrix3s R = U * V.transpose();
  if (R.determinant() < 0)
  {
    Eigen::Matrix3s scales = Eigen::Matrix3s::Identity();
    scales(2, 2) = -1;
    R = U * scales * V.transpose();
  }
  Eigen::Vector3s translation = worldCentroid - R * localCentroid;

  Eigen::Isometry3s transform = Eigen::Isometry3s::Identity();
  transform.linear() = R;
  transform.translation() = translation;
  return transform;
}

//==============================================================================
/// This implements the method in "Constrained least-squares optimization for
/// robust estimation of center of rotation" by Chang and Pollard, 2006. This
/// is the simpler version, that only supports a single marker.
///
/// This assumes we're operating in a frame where the joint center is fixed in
/// place, and `markerTrace` is a list of marker positions over time in that
/// frame. So `markerTrace[t]` is the location of the marker on its t'th
/// observation. This method will return the joint center in that frame.
///
/// The benefit of using this instead of the
/// `closedFormPivotFindingJointCenterSolver` is that we can run it on pairs
/// of bodies where only one side of the pair has 3 markers on it (commonly,
/// the ankle).
///
/// IMPORTANT NOTE: This method assumes there are no markers that are
/// literally coincident with the joint center. It has a singularity in that
/// case, and will produce garbage. However, the only reason we would have a
/// marker on top of the joint center is if someone actually gave us marker
/// data with virtual joint centers already computed, in which case maybe we
/// just respect their wishes?
Eigen::Vector3s IKInitializer::getChangPollard2006JointCenterSingleMarker(
    std::vector<Eigen::Vector3s> markerTrace, bool log)
{
  // Construct the matrix D, described in Equation 12 in the paper
  Eigen::MatrixXs D = Eigen::MatrixXs::Zero(markerTrace.size(), 5);
  for (int i = 0; i < markerTrace.size(); i++)
  {
    D(i, 0) = markerTrace[i].squaredNorm();
    D(i, 1) = markerTrace[i](0);
    D(i, 2) = markerTrace[i](1);
    D(i, 3) = markerTrace[i](2);
    D(i, 4) = 1.0;
  }
  if (log)
  {
    std::cout << "The data matrix D: " << std::endl << D << std::endl;
  }

  // Construct the matrix S, described in Equation 13 in the paper
  Eigen::MatrixXs S = D.transpose() * D;
  if (log)
  {
    std::cout << "The objective matrix S: " << std::endl << S << std::endl;
  }

  // Construct the constraint matrix C, described in Equation 14 in the paper
  Eigen::MatrixXs C = Eigen::MatrixXs::Zero(5, 5);
  // C(0, 0) = 1;
  C(4, 0) = -2;
  C(0, 4) = -2;
  C(1, 1) = 1;
  C(2, 2) = 1;
  C(3, 3) = 1;
  if (log)
  {
    std::cout << "Constraint matrix C: " << std::endl << C << std::endl;
  }

  // Create a GeneralizedEigenSolver object
  Eigen::GeneralizedEigenSolver<Eigen::MatrixXs> solver;

  // Compute the generalized eigenvalues and eigenvectors
  solver.compute(S, C);

  auto result = solver.info();
  if (result == Eigen::NumericalIssue)
  {
    std::cout << "GeneralizedEigenSolver failed with Eigen::NumericalIssue"
              << std::endl;
  }
  else if (result == Eigen::NoConvergence)
  {
    std::cout << "GeneralizedEigenSolver failed with Eigen::NoConvergence"
              << std::endl;
  }
  else if (result == Eigen::InvalidInput)
  {
    std::cout << "GeneralizedEigenSolver failed with Eigen::InvalidInput"
              << std::endl;
  }
  assert(result == Eigen::Success);

  // Print the generalized eigenvalues - we use the opposite sign convention for
  // the eigenvalues as the solver
  Eigen::VectorXcd complexEigenvalues
      = solver.alphas().cwiseQuotient(solver.betas()).transpose();
  if (log)
  {
    std::cout << "The generalized eigenvalues of S and C are:\n"
              << complexEigenvalues << std::endl;
  }

  // Print the generalized eigenvectors
  Eigen::MatrixXcd complexEigenvectors = solver.eigenvectors();
  if (log)
  {
    std::cout << "A matrix whose columns are eigenvectors corresponding to"
              << "\nthe generalized eigenvalues of S and C are:\n"
              << complexEigenvectors << std::endl;
  }

  // Reconstruct the solution from the eigenvectors, described after equation 17
  // in the paper. To quote: "The best-fit solution is the generalized
  // eigenvector u with non-negative eigenvalue l which has the least cost
  // according to Eq. (13) and subject to Eq. (14)"
  s_t bestCost = std::numeric_limits<s_t>::infinity();
  Eigen::Vector3s reconstructedCenter = Eigen::Vector3s::Zero();
  for (int i = 0; i < complexEigenvalues.size(); i++)
  {
    if (complexEigenvalues(i).imag() != 0
        || complexEigenvalues(i).real() <= 1e-12)
      continue;
    Eigen::VectorXs eigenvector = complexEigenvectors.col(i).real();

    // Normalized by constraint according to Equation 14 in the paper
    s_t constraint = eigenvector.dot(C * eigenvector);
    s_t scaleBy = sqrt(1.0 / constraint);
    if (!isfinite(scaleBy))
      continue;
    Eigen::VectorXs u = eigenvector * scaleBy;
    if (u.hasNaN())
      continue;
    s_t a = u(0);
    // This means we're in a numerically unstable fit, due to extremely low
    // marker noise (in practice this pretty much only happens on synthetic
    // data)
    if (std::abs(u(0)) < 1e-8)
    {
      continue;
    }

    if (log)
    {
      std::cout << "Un-Normalized result [" << i << "]: " << std::endl
                << eigenvector << std::endl;
      std::cout << "Constraint [" << i
                << "] (will be normalized to 1.0): " << constraint << std::endl;
      std::cout << "Normalized result [" << i << "]: " << std::endl
                << u << std::endl;
    }

#ifndef NDEBUG
    s_t eigenvalue = complexEigenvalues(i).real();
    // Double check that the rescaled eigenvector still satisfies the equation
    // (it must, cause everything is linear)
    assert(((S * u) - (eigenvalue * C * u)).norm() < 1e-8);
#endif

#ifndef NDEBUG
    if (log)
      std::cout << "Normalized Constraint [" << i << "]: " << u.dot(C * u)
                << std::endl;
    assert(abs(u.dot(C * u) - 1.0) < 1e-6);
#endif

    // Cost according to Equation 13 in the paper
    s_t cost = u.dot(S * u);
    cost /= a * a;
    if (log)
      std::cout << "Cost[" << i << "]: " << cost << std::endl;
    Eigen::Vector3s center = u.segment<3>(1) / (-2.0 * a);
    if (log)
      std::cout << "Center[" << i << "]: " << std::endl << center << std::endl;

#ifndef NDEBUG
    s_t radiusSquared = abs(u(4) / a - center.squaredNorm());
    s_t reconstructedCost = 0.0;
    for (int i = 0; i < markerTrace.size(); i++)
    {
      s_t distance = (markerTrace[i] - center).squaredNorm() - radiusSquared;
      reconstructedCost += distance * distance;
    }
    if (log)
    {
      std::cout << "Reconstructed Cost[" << i << "]: " << reconstructedCost
                << std::endl;
    }
    // Don't run this test if these numbers are near a singularity
    if ((abs(cost) + abs(reconstructedCost)) < 1e8)
    {
      assert(
          abs(cost - reconstructedCost) / (abs(cost) + abs(reconstructedCost))
          < 1e-6);
    }
    if (log)
    {
      std::cout << "Radius[" << i << "]: " << sqrt(radiusSquared) << std::endl;
    }
#endif

    if (cost < bestCost)
    {
      bestCost = cost;
      reconstructedCenter = center;
    }
  }

  if (!std::isfinite(bestCost))
  {
    // We fall back to a least-squares fit when there's no valid solution, which
    // generally means that there was no noise (and therefore no radius
    // variability) in the marker data.
    reconstructedCenter = leastSquaresSphereFit(markerTrace);
  }

  return reconstructedCenter;
}

/// This implements the variant of ChangPollard2006 that supports multiple
/// markers.
Eigen::Vector3s IKInitializer::getChangPollard2006JointCenterMultiMarker(
    std::vector<std::vector<Eigen::Vector3s>> markerTraces, bool log)
{
  int numMarkers = markerTraces.size();
  int uDim = 4 + numMarkers;

  std::vector<Eigen::MatrixXs> Ds;
  int sumRows = 0;
  for (auto& markerTrace : markerTraces)
  {
    // Construct the matrix D, described in Equation 12 in the paper
    Eigen::MatrixXs D = Eigen::MatrixXs::Zero(markerTrace.size(), 4);
    for (int i = 0; i < markerTrace.size(); i++)
    {
      D(i, 0) = markerTrace[i].squaredNorm();
      D(i, 1) = markerTrace[i](0);
      D(i, 2) = markerTrace[i](1);
      D(i, 3) = markerTrace[i](2);
    }
    if (log)
    {
      std::cout << "The data matrix D[" << Ds.size() << "]: " << std::endl
                << D << std::endl;
    }
    Ds.push_back(D);
    sumRows += D.rows();
  }
  Eigen::MatrixXs D = Eigen::MatrixXs::Zero(sumRows, uDim);
  int rowCursor = 0;
  for (int i = 0; i < Ds.size(); i++)
  {
    D.block(rowCursor, 0, Ds[i].rows(), Ds[i].cols()) = Ds[i];
    D.block(rowCursor, 4 + i, Ds[i].rows(), 1).setConstant(1.0);
    rowCursor += Ds[i].rows();
  }
  if (log)
  {
    std::cout << "The assembled data matrix D: " << std::endl << D << std::endl;
  }

  // Construct the matrix S, described in Equation 13 in the paper
  Eigen::MatrixXs S = D.transpose() * D;
  if (log)
  {
    std::cout << "The objective matrix S: " << std::endl << S << std::endl;
  }

  // Construct the constraint matrix C, described in Equation 14 in the paper
  Eigen::MatrixXs C = Eigen::MatrixXs::Zero(uDim, uDim);
  // C(0, 0) = 1;
  C(1, 1) = numMarkers;
  C(2, 2) = numMarkers;
  C(3, 3) = numMarkers;
  for (int i = 4; i < uDim; i++)
  {
    C(i, 0) = -2;
    C(0, i) = -2;
  }
  if (log)
  {
    std::cout << "Constraint matrix C: " << std::endl << C << std::endl;
  }

  // Create a GeneralizedEigenSolver object
  Eigen::GeneralizedEigenSolver<Eigen::MatrixXs> solver;

  // Compute the generalized eigenvalues and eigenvectors
  solver.compute(S, C);

  auto result = solver.info();
  if (result == Eigen::NumericalIssue)
  {
    std::cout << "GeneralizedEigenSolver failed with Eigen::NumericalIssue"
              << std::endl;
  }
  else if (result == Eigen::NoConvergence)
  {
    std::cout << "GeneralizedEigenSolver failed with Eigen::NoConvergence"
              << std::endl;
  }
  else if (result == Eigen::InvalidInput)
  {
    std::cout << "GeneralizedEigenSolver failed with Eigen::InvalidInput"
              << std::endl;
  }
  assert(result == Eigen::Success);

  // Print the generalized eigenvalues - we use the opposite sign convention for
  // the eigenvalues as the solver
  Eigen::VectorXcd complexEigenvalues
      = solver.alphas().cwiseQuotient(solver.betas()).transpose();
  if (log)
  {
    std::cout << "The generalized eigenvalues of S and C are:\n"
              << complexEigenvalues << std::endl;
  }

  // Print the generalized eigenvectors
  Eigen::MatrixXcd complexEigenvectors = solver.eigenvectors();
  if (log)
  {
    std::cout << "A matrix whose columns are eigenvectors corresponding to"
              << "\nthe generalized eigenvalues of S and C are:\n"
              << complexEigenvectors << std::endl;
  }

  // Reconstruct the solution from the eigenvectors, described after equation 17
  // in the paper. To quote: "The best-fit solution is the generalized
  // eigenvector u with non-negative eigenvalue l which has the least cost
  // according to Eq. (13) and subject to Eq. (14)"
  s_t bestCost = std::numeric_limits<s_t>::infinity();
  Eigen::Vector3s reconstructedCenter = Eigen::Vector3s::Zero();
  for (int i = 0; i < complexEigenvalues.size(); i++)
  {
    if (complexEigenvalues(i).imag() != 0
        || complexEigenvalues(i).real() <= 1e-12)
      continue;
    Eigen::VectorXs eigenvector = complexEigenvectors.col(i).real();

    // Normalized by constraint according to Equation 14 in the paper
    s_t constraint = eigenvector.dot(C * eigenvector);
    s_t scaleBy = sqrt(1.0 / constraint);
    if (!isfinite(scaleBy))
      continue;
    Eigen::VectorXs u = eigenvector * scaleBy;
    if (u.hasNaN())
      continue;
    s_t a = u(0);
    // This means we're in a numerically unstable fit, due to extremely low
    // marker noise (in practice this pretty much only happens on synthetic
    // data)
    if (std::abs(u(0)) < 1e-8)
    {
      continue;
    }

    if (log)
    {
      std::cout << "Un-Normalized result [" << i << "]: " << std::endl
                << eigenvector << std::endl;
      std::cout << "Constraint [" << i
                << "] (will be normalized to 1.0): " << constraint << std::endl;
      std::cout << "Normalized result [" << i << "]: " << std::endl
                << u << std::endl;
    }

#ifndef NDEBUG
    s_t eigenvalue = complexEigenvalues(i).real();
    // Double check that the rescaled eigenvector still satisfies the equation
    // (it must, cause everything is linear)
    assert(((S * u) - (eigenvalue * C * u)).norm() < 1e-8);
#endif

#ifndef NDEBUG
    if (log)
      std::cout << "Normalized Constraint [" << i << "]: " << u.dot(C * u)
                << std::endl;
    assert(abs(u.dot(C * u) - 1.0) < 1e-6);
#endif

    // Cost according to Equation 13 in the paper
    s_t cost = u.dot(S * u);
    cost /= a * a;
    if (log)
      std::cout << "Cost[" << i << "]: " << cost << std::endl;
    Eigen::Vector3s center = u.segment<3>(1) / (-2.0 * a);
    if (log)
      std::cout << "Center[" << i << "]: " << std::endl << center << std::endl;

    if (cost < bestCost)
    {
      bestCost = cost;
      reconstructedCenter = center;
    }
  }

  if (!std::isfinite(bestCost))
  {
    // We fall back to a least-squares fit when there's no valid solution, which
    // generally means that there was no noise (and therefore no radius
    // variability) in the marker data.
    reconstructedCenter = leastSquaresConcentricSphereFit(markerTraces);
  }

  return reconstructedCenter;
}

/// This implements a simple least-squares problem to find the center of a
/// sphere of unknown radius, with samples along the hull given by `points`.
/// This tolerates noise less well than the ChangPollard2006 method, because
/// it biases the radius of the sphere towards zero in the presence of
/// ambiguity, because it is part of the least-squares terms. However, this
/// method will work even on data with zero noise, whereas ChangPollard2006
/// will fail when there is zero noise (for example, on synthetic datasets).
Eigen::Vector3s IKInitializer::leastSquaresSphereFit(
    std::vector<Eigen::Vector3s> points)
{
  // Method copied from https://jekel.me/2015/Least-Squares-Sphere-Fit/
  Eigen::VectorXs f = Eigen::VectorXs::Zero(points.size());
  Eigen::MatrixXs A = Eigen::MatrixXs::Zero(points.size(), 4);

  for (int i = 0; i < points.size(); i++)
  {
    Eigen::Vector3s p = points[i];
    f(i) = p.squaredNorm();

    A(i, 0) = 2 * p(0);
    A(i, 1) = 2 * p(1);
    A(i, 2) = 2 * p(2);
    A(i, 3) = 1;
  }

  Eigen::VectorXs c = A.completeOrthogonalDecomposition().solve(f);
  Eigen::Vector3s center = c.head<3>();
  return center;
}

/// This is just like a leastSquaresSphereFit(), but it allows for multiple
/// radii, and finds the best fit for the center of a spheres with the same
/// center.
Eigen::Vector3s IKInitializer::leastSquaresConcentricSphereFit(
    std::vector<std::vector<Eigen::Vector3s>> traces)
{
  int dim = 0;
  for (auto& trace : traces)
    dim += trace.size();

  Eigen::VectorXs f = Eigen::VectorXs::Zero(dim);
  Eigen::MatrixXs A = Eigen::MatrixXs::Zero(dim, 3 + traces.size());
  int rowCursor = 0;
  int colCursor = 3;
  for (auto& trace : traces)
  {
    for (Eigen::Vector3s& p : trace)
    {
      f(rowCursor) = p.squaredNorm();

      A(rowCursor, 0) = 2 * p(0);
      A(rowCursor, 1) = 2 * p(1);
      A(rowCursor, 2) = 2 * p(2);
      A(rowCursor, colCursor) = 1;

      rowCursor++;
    }
    colCursor++;
  }
  Eigen::VectorXs c = A.completeOrthogonalDecomposition().solve(f);
  Eigen::Vector3s center = c.head<3>();
  return center;
}

/// This will attempt to fit an axis to a set of traces, after we've already
/// found the center of rotation, by running an SVD on a matrix composed of
/// the traces and looking at the near-zero singular value, if there is one.
/// This returns the axis fit, along with the corresponding singular value (if
/// that number is too high, the axis should be disregarded).
std::pair<Eigen::Vector3s, s_t> IKInitializer::svdAxisFit(
    std::vector<std::vector<Eigen::Vector3s>> traces,
    Eigen::Vector3s jointCenter)
{
  std::vector<Eigen::Vector3s> traceAvg;
  int totalPoints = 0;
  for (auto& trace : traces)
  {
    Eigen::Vector3s avg = Eigen::Vector3s::Zero();
    for (auto& point : trace)
    {
      avg += (point - jointCenter);
    }
    avg /= trace.size();
    traceAvg.push_back(avg);
    totalPoints += trace.size();
  }
  Eigen::MatrixXs points = Eigen::MatrixXs::Zero(totalPoints, 3);
  int rowCursor = 0;
  for (int i = 0; i < traces.size(); i++)
  {
    for (int j = 0; j < traces[i].size(); j++)
    {
      points.row(rowCursor) = (traces[i][j] - jointCenter) - traceAvg[i];
      rowCursor++;
    }
  }
  Eigen::BDCSVD<Eigen::MatrixXs> svd(
      points, Eigen::ComputeThinU | Eigen::ComputeThinV);
  Eigen::Vector3s axis = svd.matrixV().col(2);
  s_t singularValue = svd.singularValues()(2);
  return std::make_pair(axis, singularValue);
}

/// This implements the least-squares method in Gamage and Lasenby 2002, "New
/// least squares solutions for estimating the average centre of rotation and
/// the axis of rotation"
std::pair<Eigen::Vector3s, s_t> IKInitializer::gamageLasenby2002AxisFit(
    std::vector<std::vector<Eigen::Vector3s>> traces)
{
  Eigen::MatrixXs A = Eigen::Matrix3s::Zero();
  for (std::vector<Eigen::Vector3s> trace : traces)
  {
    Eigen::Vector3s mean = Eigen::Vector3s::Zero();
    Eigen::Matrix3s meanOuterProduct = Eigen::Matrix3s::Zero();
    for (Eigen::Vector3s& point : trace)
    {
      mean += point;
      meanOuterProduct += point * point.transpose();
    }
    mean /= trace.size();
    meanOuterProduct /= trace.size();

    A += meanOuterProduct - (mean * mean.transpose());
  }

  Eigen::JacobiSVD<Eigen::MatrixXs> svd(
      A, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Eigen::Vector3s axis = svd.matrixV().col(2);
  s_t singularValue = svd.singularValues()(2);
  return std::make_pair(axis, singularValue);
}

} // namespace biomechanics
} // namespace dart