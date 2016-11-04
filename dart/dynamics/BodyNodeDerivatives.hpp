/*
 * Copyright (c) 2016, Graphics Lab, Georgia Tech Research Corporation
 * Copyright (c) 2016, Humanoid Lab, Georgia Tech Research Corporation
 * Copyright (c) 2016, Personal Robotics Lab, Carnegie Mellon University
 * All rights reserved.
 *
 * This file is provided under the following "BSD-style" License:
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef DART_DYNAMICS_BODYNODEDERIVATIVES_HPP_
#define DART_DYNAMICS_BODYNODEDERIVATIVES_HPP_

#include <Eigen/Dense>

#include "dart/common/AspectWithVersion.hpp"
#include "dart/dynamics/Skeleton.hpp"

namespace dart {
namespace dynamics {

class BodyNodeDerivatives final :
    public common::CompositeTrackingAspect<BodyNode>
{
public:

  friend class SkeletonDerivatives;

  using Base = common::CompositeTrackingAspect<BodyNode>;
  using GradientMatrix = Eigen::Matrix<double, 6, Eigen::Dynamic>;

  BodyNodeDerivatives() = default;

  BodyNodeDerivatives(const BodyNodeDerivatives&) = delete;

  // Documentation inherited
  std::unique_ptr<Aspect> cloneAspect() const override;

  //----------------------------------------------------------------------------
  /// \{ \name Derivative of spatial velocity
  //----------------------------------------------------------------------------

  const GradientMatrix& getSpatialVelocityDerivativeWrtPositions() const;

  Eigen::Vector6d getSpatialVelocityDerivativeWrtPositions(
      std::size_t indexInSkeleton) const;

  Eigen::Vector6d getSpatialVelocityDerivativeWrtPositions(
      const DegreeOfFreedom* withRespectTo) const;

  GradientMatrix getSpatialVelocityDerivativeWrtPositions(
      const Joint* withRespectTo) const;

  const GradientMatrix& getSpatialVelocityDerivativeWrtVelocities() const;

  Eigen::Vector6d getSpatialVelocityDerivativeWrtVelocities(
      std::size_t indexInSkeleton) const;

  Eigen::Vector6d getSpatialVelocityDerivativeWrtVelocities(
      const DegreeOfFreedom* withRespectTo) const;

  GradientMatrix getSpatialVelocityDerivativeWrtVelocities(
      const Joint* withRespectTo) const;

  /// \}

  // TODO(JS): Derivative of spatial acceleration

  //----------------------------------------------------------------------------
  /// \{ \name Gradients of Lagrangian
  //----------------------------------------------------------------------------

  Eigen::VectorXd computeKineticEnergyGradientWrtPositions() const;
  Eigen::VectorXd computeKineticEnergyGradientWrtVelocities() const;

  Eigen::VectorXd computeLagrangianGradientWrtPositions() const;
  Eigen::VectorXd computeLagrangianGradientWrtVelocities() const;

  /// \}

protected:

  void setComposite(common::Composite* newComposite) override;

  void dirtySpatialVelocityDerivativeWrtPositions();
  void dirtySpatialVelocityDerivativeWrtVelocities();

protected:

  /// \warning Do not use directly! Use
  /// getSpatialVelocityDerivativeWrtPositions() to access this quantity.
  mutable GradientMatrix mV_q{GradientMatrix()};

  /// \warning Do not use directly! Use
  /// getSpatialVelocityDerivativeWrtVelocities() to access this quantity.
  mutable GradientMatrix mV_dq{GradientMatrix()};

  /// \warning Do not use directly! Use
  /// getSpatialVelocityDerivativeWrtPositions() to access this quantity.
  mutable GradientMatrix mdV_q{GradientMatrix()};

  /// \warning Do not use directly! Use
  /// getSpatialVelocityDerivativeWrtVelocities() to access this quantity.
  mutable GradientMatrix mdV_dq{GradientMatrix()};

  mutable bool mNeedSpatialVelocityDerivativeWrtPositionsUpdate{true};
  mutable bool mNeedSpatialVelocityDerivativeWrtVelocitiesUpdate{true};
  mutable bool mNeedSpatialAccelerationDerivativeWrtPositionsUpdate{true};
  mutable bool mNeedSpatialAccelerationDerivativeWrtVelocitiesUpdate{true};

};

}  // namespace dynamics
}  // namespace dart

#endif  // DART_DYNAMICS_BODYNODEDERIVATIVES_HPP_
