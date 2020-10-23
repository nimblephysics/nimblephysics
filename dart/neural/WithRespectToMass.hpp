#ifndef DART_NEURAL_WRT_MASS_HPP_
#define DART_NEURAL_WRT_MASS_HPP_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <Eigen/Dense>

#include "dart/neural/WithRespectTo.hpp"

namespace dart {
namespace simulation {
class World;
}

namespace dynamics {
class Skeleton;
class BodyNode;
} // namespace dynamics

namespace neural {

enum WrtMassBodyNodeEntryType
{
  INERTIA_MASS,
  INERTIA_COM,
  INERTIA_DIAGONAL,
  INERTIA_OFF_DIAGONAL,
  INERTIA_FULL
};

struct WrtMassBodyNodyEntry
{
  std::string linkName;
  WrtMassBodyNodeEntryType type;

  WrtMassBodyNodyEntry(std::string linkName, WrtMassBodyNodeEntryType type);

  int dim();

  void get(dynamics::Skeleton* skel, Eigen::Ref<Eigen::VectorXd> out);

  void set(dynamics::Skeleton* skel, const Eigen::Ref<Eigen::VectorXd>& val);
};

class WithRespectToMass : public WithRespectTo
{
public:
  WithRespectToMass();

  /// This registers that we'd like to keep track of this node's mass in this
  /// way in this differentiation
  void registerNode(dynamics::BodyNode* node, WrtMassBodyNodeEntryType type);

  //////////////////////////////////////////////////////////////
  // Implement all the methods we need
  //////////////////////////////////////////////////////////////

  /// This returns this WRT from the world as a vector
  Eigen::VectorXd get(std::shared_ptr<simulation::World> world) override;

  /// This returns this WRT from a skeleton as a vector
  Eigen::VectorXd get(dynamics::Skeleton* skel) override;

  /// This sets the world's state based on our WRT
  void set(
      std::shared_ptr<simulation::World> world, Eigen::VectorXd value) override;

  /// This sets the skeleton's state based on our WRT
  void set(dynamics::Skeleton* skel, Eigen::VectorXd value) override;

  /// This gives the dimensions of the WRT
  int dim(std::shared_ptr<simulation::World> world) override;

  /// This gives the dimensions of the WRT in a single skeleton
  int dim(dynamics::Skeleton* skel) override;

protected:
  std::unordered_map<std::string, std::vector<WrtMassBodyNodyEntry>> mEntries;
};

} // namespace neural
} // namespace dart

#endif