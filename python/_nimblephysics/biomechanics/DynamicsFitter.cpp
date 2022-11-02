#include <memory>

#include <Eigen/Dense>
#include <dart/biomechanics/DynamicsFitter.hpp>
#include <dart/biomechanics/MarkerFitter.hpp>
#include <dart/dynamics/BodyNode.hpp>
#include <dart/dynamics/Skeleton.hpp>
#include <pybind11/eigen.h>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace dart {
namespace python {

void DynamicsFitter(py::module& m)
{
  ::py::class_<dart::biomechanics::ResidualForceHelper>(
      m, "ResidualForceHelper")
      .def(
          ::py::init<std::shared_ptr<dynamics::Skeleton>, std::vector<int>>(),
          ::py::arg("skeleton"),
          ::py::arg("forceBodies"))
      .def(
          "calculateResidual",
          &dart::biomechanics::ResidualForceHelper::calculateResidual,
          ::py::arg("q"),
          ::py::arg("dq"),
          ::py::arg("ddq"),
          ::py::arg("forcesConcat"))
      .def(
          "calculateResidualNorm",
          &dart::biomechanics::ResidualForceHelper::calculateResidualNorm,
          ::py::arg("q"),
          ::py::arg("dq"),
          ::py::arg("ddq"),
          ::py::arg("forcesConcat"),
          ::py::arg("torquesMultiple"),
          ::py::arg("useL1") = false)
      .def(
          "calculateResidualJacobianWrt",
          &dart::biomechanics::ResidualForceHelper::
              calculateResidualJacobianWrt,
          ::py::arg("q"),
          ::py::arg("dq"),
          ::py::arg("ddq"),
          ::py::arg("forcesConcat"),
          ::py::arg("wrt"))
      .def(
          "calculateResidualNormGradientWrt",
          &dart::biomechanics::ResidualForceHelper::
              calculateResidualNormGradientWrt,
          ::py::arg("q"),
          ::py::arg("dq"),
          ::py::arg("ddq"),
          ::py::arg("forcesConcat"),
          ::py::arg("wrt"),
          ::py::arg("torquesMultiple"),
          ::py::arg("useL1") = false);

  ::py::class_<
      dart::biomechanics::DynamicsInitialization,
      std::shared_ptr<dart::biomechanics::DynamicsInitialization>>(
      m, "DynamicsInitialization")
      .def_readwrite(
          "forcePlateTrials",
          &dart::biomechanics::DynamicsInitialization::forcePlateTrials)
      .def_readwrite(
          "markerObservationTrials",
          &dart::biomechanics::DynamicsInitialization::markerObservationTrials)
      .def_readwrite(
          "trialTimesteps",
          &dart::biomechanics::DynamicsInitialization::trialTimesteps)
      .def_readwrite(
          "grfTrials", &dart::biomechanics::DynamicsInitialization::grfTrials)
      .def_readwrite(
          "grfBodyIndices",
          &dart::biomechanics::DynamicsInitialization::grfBodyIndices)
      .def_readwrite(
          "grfBodyNodes",
          &dart::biomechanics::DynamicsInitialization::grfBodyNodes)
      .def_readwrite(
          "groundHeight",
          &dart::biomechanics::DynamicsInitialization::groundHeight)
      .def_readwrite(
          "flatGround", &dart::biomechanics::DynamicsInitialization::flatGround)
      .def_readwrite(
          "contactBodies",
          &dart::biomechanics::DynamicsInitialization::contactBodies)
      .def_readwrite(
          "grfBodyContactSphereRadius",
          &dart::biomechanics::DynamicsInitialization::
              grfBodyContactSphereRadius)
      .def_readwrite(
          "grfBodyForceActive",
          &dart::biomechanics::DynamicsInitialization::grfBodyForceActive)
      .def_readwrite(
          "grfBodySphereInContact",
          &dart::biomechanics::DynamicsInitialization::grfBodySphereInContact)
      .def_readwrite(
          "defaultForcePlateCorners",
          &dart::biomechanics::DynamicsInitialization::defaultForcePlateCorners)
      .def_readwrite(
          "grfBodyOffForcePlate",
          &dart::biomechanics::DynamicsInitialization::grfBodyOffForcePlate)
      .def_readwrite(
          "probablyMissingGRF",
          &dart::biomechanics::DynamicsInitialization::probablyMissingGRF)
      .def_readwrite(
          "bodyMasses", &dart::biomechanics::DynamicsInitialization::bodyMasses)
      .def_readwrite(
          "bodyCom", &dart::biomechanics::DynamicsInitialization::bodyCom)
      .def_readwrite(
          "bodyInertia",
          &dart::biomechanics::DynamicsInitialization::bodyInertia)
      .def_readwrite(
          "poseTrials", &dart::biomechanics::DynamicsInitialization::poseTrials)
      .def_readwrite(
          "groupScales",
          &dart::biomechanics::DynamicsInitialization::groupScales)
      .def_readwrite(
          "markerOffsets",
          &dart::biomechanics::DynamicsInitialization::markerOffsets)
      .def_readwrite(
          "trackingMarkers",
          &dart::biomechanics::DynamicsInitialization::trackingMarkers)
      .def_readwrite(
          "joints", &dart::biomechanics::DynamicsInitialization::joints)
      .def_readwrite(
          "jointsAdjacentMarkers",
          &dart::biomechanics::DynamicsInitialization::jointsAdjacentMarkers)
      .def_readwrite(
          "jointWeights",
          &dart::biomechanics::DynamicsInitialization::jointWeights)
      .def_readwrite(
          "jointCenters",
          &dart::biomechanics::DynamicsInitialization::jointCenters)
      .def_readwrite(
          "axisWeights",
          &dart::biomechanics::DynamicsInitialization::axisWeights)
      .def_readwrite(
          "jointAxis", &dart::biomechanics::DynamicsInitialization::jointAxis)
      .def_readwrite(
          "updatedMarkerMap",
          &dart::biomechanics::DynamicsInitialization::updatedMarkerMap)
      .def_readwrite(
          "originalPoses",
          &dart::biomechanics::DynamicsInitialization::originalPoses)
      .def_readwrite(
          "originalGroupMasses",
          &dart::biomechanics::DynamicsInitialization::originalGroupMasses)
      .def_readwrite(
          "originalGroupCOMs",
          &dart::biomechanics::DynamicsInitialization::originalGroupCOMs)
      .def_readwrite(
          "originalGroupInertias",
          &dart::biomechanics::DynamicsInitialization::originalGroupInertias)
      .def_readwrite(
          "originalGroupScales",
          &dart::biomechanics::DynamicsInitialization::originalGroupScales)
      .def_readwrite(
          "originalMarkerOffsets",
          &dart::biomechanics::DynamicsInitialization::originalMarkerOffsets);

  /*
class DynamicsFitter
{
public:
  // This debugs the current state, along with visualizations of errors where
  // the dynamics do not match the force plate data
  void saveDynamicsToGUI(
      const std::string& path,
      std::shared_ptr<DynamicsInitialization> init,
      int trialIndex,
      int framesPerSecond);

  void setTolerance(double tol);
  void setIterationLimit(int limit);
  void setLBFGSHistoryLength(int len);
  void setCheckDerivatives(bool check);
  void setPrintFrequency(int freq);
  void setSilenceOutput(bool silent);
  void setDisableLinesearch(bool disable);

protected:
  std::shared_ptr<dynamics::Skeleton> mSkeleton;
  std::vector<dynamics::BodyNode*> mFootNodes;
  dynamics::MarkerMap mMarkerMap;
  std::vector<std::string> mTrackingMarkers;
  // These are IPOPT settings
  double mTolerance;
  int mIterationLimit;
  int mLBFGSHistoryLength;
  bool mCheckDerivatives;
  int mPrintFrequency;
  bool mSilenceOutput;
  bool mDisableLinesearch;
};
  */
  /*
  DynamicsFitProblemConfig(std::shared_ptr<dynamics::Skeleton> skeleton);
  DynamicsFitProblemConfig& setIncludeMasses(bool value);
  DynamicsFitProblemConfig& setIncludeCOMs(bool value);
  DynamicsFitProblemConfig& setIncludeInertias(bool value);
  DynamicsFitProblemConfig& setIncludePoses(bool value);
  DynamicsFitProblemConfig& setIncludeMarkerOffsets(bool value);
  DynamicsFitProblemConfig& setIncludeBodyScales(bool value);

  DynamicsFitProblemConfig& setLinearNewtonWeight(s_t weight);
  DynamicsFitProblemConfig& setResidualWeight(s_t weight);
  DynamicsFitProblemConfig& setMarkerWeight(s_t weight);
  DynamicsFitProblemConfig& setJointWeight(s_t weight);

  DynamicsFitProblemConfig& setLinearNewtonUseL1(bool l1);
  DynamicsFitProblemConfig& setResidualUseL1(bool l1);
  DynamicsFitProblemConfig& setMarkerUseL1(bool l1);

  DynamicsFitProblemConfig& setRegularizeSpatialAcc(s_t value);
  DynamicsFitProblemConfig& setRegularizeSpatialAccBodyWeights(
      Eigen::VectorXs bodyWeights);
  DynamicsFitProblemConfig& setRegularizeSpatialAccUseL1(bool l1);

  DynamicsFitProblemConfig& setResidualTorqueMultiple(s_t value);
  DynamicsFitProblemConfig& setRegularizeMasses(s_t value);
  DynamicsFitProblemConfig& setRegularizeCOMs(s_t value);
  DynamicsFitProblemConfig& setRegularizeInertias(s_t value);
  DynamicsFitProblemConfig& setRegularizeBodyScales(s_t value);
  DynamicsFitProblemConfig& setRegularizePoses(s_t value);
  DynamicsFitProblemConfig& setRegularizeTrackingMarkerOffsets(s_t value);
  DynamicsFitProblemConfig& setRegularizeAnatomicalMarkerOffsets(s_t value);
  DynamicsFitProblemConfig& setRegularizeImpliedDensity(s_t value);

  DynamicsFitProblemConfig& setVelAccImplicit(bool implicit);
  */
  ::py::class_<dart::biomechanics::DynamicsFitProblemConfig>(
      m, "DynamicsFitProblemConfig")
      .def(
          ::py::init<std::shared_ptr<dynamics::Skeleton>>(),
          ::py::arg("skeleton"))
      .def(
          "setIncludeMasses",
          &dart::biomechanics::DynamicsFitProblemConfig::setIncludeMasses,
          ::py::arg("value"))
      .def(
          "setIncludeCOMs",
          &dart::biomechanics::DynamicsFitProblemConfig::setIncludeCOMs,
          ::py::arg("value"))
      .def(
          "setIncludeInertias",
          &dart::biomechanics::DynamicsFitProblemConfig::setIncludeInertias,
          ::py::arg("value"))
      .def(
          "setIncludePoses",
          &dart::biomechanics::DynamicsFitProblemConfig::setIncludePoses,
          ::py::arg("value"))
      .def(
          "setIncludeMarkerOffsets",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setIncludeMarkerOffsets,
          ::py::arg("value"))
      .def(
          "setIncludeBodyScales",
          &dart::biomechanics::DynamicsFitProblemConfig::setIncludeBodyScales,
          ::py::arg("value"))
      .def(
          "setLinearNewtonWeight",
          &dart::biomechanics::DynamicsFitProblemConfig::setLinearNewtonWeight,
          ::py::arg("value"))
      .def(
          "setResidualWeight",
          &dart::biomechanics::DynamicsFitProblemConfig::setResidualWeight,
          ::py::arg("value"))
      .def(
          "setMarkerWeight",
          &dart::biomechanics::DynamicsFitProblemConfig::setMarkerWeight,
          ::py::arg("value"))
      .def(
          "setJointWeight",
          &dart::biomechanics::DynamicsFitProblemConfig::setJointWeight,
          ::py::arg("value"))
      .def(
          "setLinearNewtonUseL1",
          &dart::biomechanics::DynamicsFitProblemConfig::setLinearNewtonUseL1,
          ::py::arg("value"))
      .def(
          "setResidualUseL1",
          &dart::biomechanics::DynamicsFitProblemConfig::setResidualUseL1,
          ::py::arg("value"))
      .def(
          "setMarkerUseL1",
          &dart::biomechanics::DynamicsFitProblemConfig::setMarkerUseL1,
          ::py::arg("value"))
      .def(
          "setRegularizeSpatialAcc",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setRegularizeSpatialAcc,
          ::py::arg("value"))
      .def(
          "setRegularizeSpatialAccUseL1",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setRegularizeSpatialAccUseL1,
          ::py::arg("value"))
      .def(
          "setRegularizeSpatialAccBodyWeights",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setRegularizeSpatialAccBodyWeights,
          ::py::arg("bodyWeights"))
      .def(
          "setResidualTorqueMultiple",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setResidualTorqueMultiple,
          ::py::arg("value"))
      .def(
          "setRegularizeMasses",
          &dart::biomechanics::DynamicsFitProblemConfig::setRegularizeMasses,
          ::py::arg("value"))
      .def(
          "setRegularizeCOMs",
          &dart::biomechanics::DynamicsFitProblemConfig::setRegularizeCOMs,
          ::py::arg("value"))
      .def(
          "setRegularizeInertias",
          &dart::biomechanics::DynamicsFitProblemConfig::setRegularizeInertias,
          ::py::arg("value"))
      .def(
          "setRegularizeBodyScales",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setRegularizeBodyScales,
          ::py::arg("value"))
      .def(
          "setRegularizePoses",
          &dart::biomechanics::DynamicsFitProblemConfig::setRegularizePoses,
          ::py::arg("value"))
      .def(
          "setRegularizeTrackingMarkerOffsets",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setRegularizeTrackingMarkerOffsets,
          ::py::arg("value"))
      .def(
          "setRegularizeAnatomicalMarkerOffsets",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setRegularizeAnatomicalMarkerOffsets,
          ::py::arg("value"))
      .def(
          "setRegularizeImpliedDensity",
          &dart::biomechanics::DynamicsFitProblemConfig::
              setRegularizeImpliedDensity,
          ::py::arg("value"))
      .def(
          "setVelAccImplicit",
          &dart::biomechanics::DynamicsFitProblemConfig::setVelAccImplicit,
          ::py::arg("implicit"));
  ;

  ::py::class_<
      dart::biomechanics::DynamicsFitter,
      std::shared_ptr<dart::biomechanics::DynamicsFitter>>(m, "DynamicsFitter")
      .def(
          ::py::init<
              std::shared_ptr<dynamics::Skeleton>,
              std::vector<dynamics::BodyNode*>,
              std::vector<std::string>>(),
          ::py::arg("skeleton"),
          ::py::arg("footNodes"),
          ::py::arg("trackingMarkers"))
      .def_static(
          "createInitialization",
          +[](std::shared_ptr<dynamics::Skeleton> skel,
              dynamics::MarkerMap markerMap,
              std::vector<std::string> trackingMarkers,
              std::vector<dynamics::BodyNode*> grfNodes,
              std::vector<std::vector<dart::biomechanics::ForcePlate>>
                  forcePlateTrials,
              std::vector<Eigen::MatrixXs> poseTrials,
              std::vector<int> framesPerSecond,
              std::vector<std::vector<std::map<std::string, Eigen::Vector3s>>>
                  markerObservationTrials)
              -> std::shared_ptr<dart::biomechanics::DynamicsInitialization> {
            return dart::biomechanics::DynamicsFitter::createInitialization(
                skel,
                markerMap,
                trackingMarkers,
                grfNodes,
                forcePlateTrials,
                poseTrials,
                framesPerSecond,
                markerObservationTrials);
          },
          ::py::arg("skel"),
          ::py::arg("markerMap"),
          ::py::arg("trackingMarkers"),
          ::py::arg("grfNodes"),
          ::py::arg("forcePlateTrials"),
          ::py::arg("poseTrials"),
          ::py::arg("framesPerSecond"),
          ::py::arg("markerObservationTrials"))
      .def_static(
          "createInitialization",
          +[](std::shared_ptr<dynamics::Skeleton> skel,
              std::vector<dart::biomechanics::MarkerInitialization>
                  kinematicInits,
              std::vector<std::string> trackingMarkers,
              std::vector<dynamics::BodyNode*> grfNodes,
              std::vector<std::vector<dart::biomechanics::ForcePlate>>
                  forcePlateTrials,
              std::vector<int> framesPerSecond,
              std::vector<std::vector<std::map<std::string, Eigen::Vector3s>>>
                  markerObservationTrials)
              -> std::shared_ptr<dart::biomechanics::DynamicsInitialization> {
            return dart::biomechanics::DynamicsFitter::createInitialization(
                skel,
                kinematicInits,
                trackingMarkers,
                grfNodes,
                forcePlateTrials,
                framesPerSecond,
                markerObservationTrials);
          },
          ::py::arg("skel"),
          ::py::arg("kinematicInits"),
          ::py::arg("trackingMarkers"),
          ::py::arg("grfNodes"),
          ::py::arg("forcePlateTrials"),
          ::py::arg("framesPerSecond"),
          ::py::arg("markerObservationTrials"))
      .def(
          "comPositions",
          &dart::biomechanics::DynamicsFitter::comPositions,
          ::py::arg("init"),
          ::py::arg("trial"))
      .def(
          "comAccelerations",
          &dart::biomechanics::DynamicsFitter::comAccelerations,
          ::py::arg("init"),
          ::py::arg("trial"))
      .def(
          "impliedCOMForces",
          &dart::biomechanics::DynamicsFitter::impliedCOMForces,
          ::py::arg("init"),
          ::py::arg("trial"),
          ::py::arg("includeGravity") = true)
      .def(
          "measuredGRFForces",
          &dart::biomechanics::DynamicsFitter::measuredGRFForces,
          ::py::arg("init"),
          ::py::arg("trial"))
      .def(
          "estimateFootGroundContacts",
          &dart::biomechanics::DynamicsFitter::estimateFootGroundContacts,
          ::py::arg("init"))
      .def(
          "smoothAccelerations",
          &dart::biomechanics::DynamicsFitter::smoothAccelerations,
          ::py::arg("init"))
      .def(
          "zeroLinearResidualsOnCOMTrajectory",
          &dart::biomechanics::DynamicsFitter::
              zeroLinearResidualsOnCOMTrajectory,
          ::py::arg("init"))
      .def(
          "optimizeSpatialResidualsOnCOMTrajectory",
          &dart::biomechanics::DynamicsFitter::
              optimizeSpatialResidualsOnCOMTrajectory,
          ::py::arg("init"))
      .def(
          "scaleLinkMassesFromGravity",
          &dart::biomechanics::DynamicsFitter::scaleLinkMassesFromGravity,
          ::py::arg("init"))
      .def(
          "estimateLinkMassesFromAcceleration",
          &dart::biomechanics::DynamicsFitter::
              estimateLinkMassesFromAcceleration,
          ::py::arg("init"),
          ::py::arg("regularizationWeight") = 50.0)
      .def(
          "runIPOPTOptimization",
          &dart::biomechanics::DynamicsFitter::runIPOPTOptimization,
          ::py::arg("init"),
          ::py::arg("config"))
      .def(
          "runSGDOptimization",
          &dart::biomechanics::DynamicsFitter::runSGDOptimization,
          ::py::arg("init"),
          ::py::arg("config"))
      .def(
          "computePerfectGRFs",
          &dart::biomechanics::DynamicsFitter::computePerfectGRFs,
          ::py::arg("init"))
      .def(
          "checkPhysicalConsistency",
          &dart::biomechanics::DynamicsFitter::checkPhysicalConsistency,
          ::py::arg("init"),
          ::py::arg("maxAcceptableErrors") = 1e-3,
          ::py::arg("maxTimestepsToTest") = 50)
      .def(
          "computeAverageMarkerRMSE",
          &dart::biomechanics::DynamicsFitter::computeAverageMarkerRMSE,
          ::py::arg("init"))
      .def(
          "computeAverageResidualForce",
          &dart::biomechanics::DynamicsFitter::computeAverageResidualForce,
          ::py::arg("init"))
      .def(
          "computeAverageRealForce",
          &dart::biomechanics::DynamicsFitter::computeAverageRealForce,
          ::py::arg("init"))
      .def(
          "computeAverageCOPChange",
          &dart::biomechanics::DynamicsFitter::computeAverageCOPChange,
          ::py::arg("init"))
      .def(
          "computeAverageForceMagnitudeChange",
          &dart::biomechanics::DynamicsFitter::
              computeAverageForceMagnitudeChange,
          ::py::arg("init"))
      .def(
          "saveDynamicsToGUI",
          &dart::biomechanics::DynamicsFitter::saveDynamicsToGUI,
          ::py::arg("path"),
          ::py::arg("init"),
          ::py::arg("trialIndex"),
          ::py::arg("framesPerSecond"))
      .def(
          "writeCSVData",
          &dart::biomechanics::DynamicsFitter::writeCSVData,
          ::py::arg("path"),
          ::py::arg("init"),
          ::py::arg("trialIndex"))
      .def(
          "setTolerance",
          &dart::biomechanics::DynamicsFitter::setTolerance,
          ::py::arg("value"))
      .def(
          "setIterationLimit",
          &dart::biomechanics::DynamicsFitter::setIterationLimit,
          ::py::arg("value"))
      .def(
          "setLBFGSHistoryLength",
          &dart::biomechanics::DynamicsFitter::setLBFGSHistoryLength,
          ::py::arg("value"))
      .def(
          "setCheckDerivatives",
          &dart::biomechanics::DynamicsFitter::setCheckDerivatives,
          ::py::arg("value"))
      .def(
          "setPrintFrequency",
          &dart::biomechanics::DynamicsFitter::setPrintFrequency,
          ::py::arg("value"))
      .def(
          "setSilenceOutput",
          &dart::biomechanics::DynamicsFitter::setSilenceOutput,
          ::py::arg("value"))
      .def(
          "setDisableLinesearch",
          &dart::biomechanics::DynamicsFitter::setDisableLinesearch,
          ::py::arg("value"));
}

} // namespace python
} // namespace dart
