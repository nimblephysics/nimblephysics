/*
 * Copyright (c) 2011-2019, The DART development contributors
 * All rights reserved.
 *
 * The list of contributors can be found at:
 *   https://github.com/dartsim/dart/blob/master/LICENSE
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

#include <dart/dart.hpp>
#include <dart/gui/osg/osg.hpp>

using namespace dart;

//==============================================================================
dynamics::SkeletonPtr createFloor()
{
  dynamics::SkeletonPtr floor = dynamics::Skeleton::create("floor");

  // Give the floor a body
  dynamics::BodyNodePtr body
      = floor->createJointAndBodyNodePair<dynamics::WeldJoint>(nullptr).second;

  // Give the body a shape
  double floorWidth = 10.0;
  double floorHeight = 0.01;
  auto box = std::make_shared<dynamics::BoxShape>(
      Eigen::Vector3d(floorWidth, floorWidth, floorHeight));
  dynamics::ShapeNode* shapeNode = body->createShapeNodeWith<
      dynamics::VisualAspect,
      dynamics::CollisionAspect,
      dynamics::DynamicsAspect>(box);
  shapeNode->getVisualAspect()->setColor(dart::Color::LightGray());

  // Put the body into position
  Eigen::Isometry3d tf = Eigen::Isometry3d::Identity();
  tf.translation() = Eigen::Vector3d(0.0, 0.0, -1.0 - floorHeight / 2.0);
  body->getParentJoint()->setTransformFromParentBodyNode(tf);

  return floor;
}

int main()
{
  auto shape
      = std::make_shared<dynamics::BoxShape>(Eigen::Vector3d(0.3, 0.3, 0.3));

  // Create a box-shaped rigid body
  auto skeleton = dynamics::Skeleton::create();
  auto jointAndBody
      = skeleton->createJointAndBodyNodePair<dynamics::FreeJoint>();
  auto body = jointAndBody.second;
  std::cerr << "body shape node count " << body->getNumShapeNodes() << std::endl;
  body->createShapeNodeWith<
      dynamics::VisualAspect,
      dynamics::CollisionAspect,
      dynamics::DynamicsAspect>(shape);
  std::cerr << "body shape node count " << body->getNumShapeNodes() << std::endl;
  std::cerr << "friction "
            << body->getShapeNode(0)->getDynamicsAspect()->getFrictionCoeff()
            << ", "
            << body->getShapeNode(0)->getDynamicsAspect()->getSecondaryFrictionCoeff()
            << std::endl;
  body->getShapeNode(0)->getDynamicsAspect()->setFrictionCoeff(0.0);
  body->getShapeNode(0)->getDynamicsAspect()->setSecondaryFrictionCoeff(1.0);
  body->getShapeNode(0)->getDynamicsAspect()->setFirstFrictionDirection(
      Eigen::Vector3d::UnitX());
  std::cerr << "friction "
            << body->getShapeNode(0)->getDynamicsAspect()->getFrictionCoeff()
            << ", "
            << body->getShapeNode(0)->getDynamicsAspect()->getSecondaryFrictionCoeff()
            << std::endl;

  // Create a world and add the rigid body
  auto world = simulation::World::create();
  std::cerr << "gravity " << world->getGravity() << std::endl;
  world->setGravity(Eigen::Vector3d(-5.0, -5.0, -9.81));
  std::cerr << "gravity " << world->getGravity() << std::endl;

  world->addSkeleton(createFloor());
  world->addSkeleton(skeleton);

  // Wrap a WorldNode around it
  ::osg::ref_ptr<gui::osg::RealTimeWorldNode> node
      = new gui::osg::RealTimeWorldNode(world);

  // Create a Viewer and set it up with the WorldNode
  auto viewer = gui::osg::Viewer();
  viewer.addWorldNode(node);

  viewer.addInstructionText("Press space to start free falling the box.\n");
  std::cout << viewer.getInstructions() << std::endl;

  // Set up the window to be 640x480
  viewer.setUpViewInWindow(0, 0, 640, 480);

  // Adjust the viewpoint of the Viewer
  viewer.getCameraManipulator()->setHomePosition(
      ::osg::Vec3(2.57f, 3.14f, 1.64f),
      ::osg::Vec3(0.00f, 0.00f, 0.00f),
      ::osg::Vec3(-0.24f, -0.25f, 0.94f));

  // We need to re-dirty the CameraManipulator by passing it into the viewer
  // again, so that the viewer knows to update its HomePosition setting
  viewer.setCameraManipulator(viewer.getCameraManipulator());

  // Begin running the application loop
  viewer.run();

  return 0;
}
