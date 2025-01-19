#!/bin/bash
set -e

# Update the pkgconfig path
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/lib64/pkgconfig/

# Install LAPACK
apt-get install libblas-dev liblapack-dev libboost-all-dev

# Install Eigen
curl https://gitlab.com/libeigen/eigen/-/archive/3.3.7/eigen-3.3.7.tar.gz > eigen.tar.gz
tar -zxf eigen.tar.gz
pushd eigen-3.3.7
mkdir build
pushd build
cmake ..
make install -j14
popd
popd
rm -rf eigen-3.3.7

# Install CCD
git clone https://github.com/danfis/libccd.git
pushd libccd
mkdir build
pushd build
cmake -DENABLE_DOUBLE_PRECISION=ON -DBUILD_SHARED_LIBS=ON ..
make && make install -j14
popd
popd
rm -rf libccd

# Install ASSIMP
git clone https://github.com/assimp/assimp.git
pushd assimp
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf assimp

# Install MUMPS
git clone https://github.com/coin-or-tools/ThirdParty-Mumps.git
pushd ThirdParty-Mumps
./get.Mumps
./configure
make -j14
make install
popd
rm -rf ThirdParty-Mumps

# Install IPOPT
git clone https://github.com/coin-or/Ipopt.git
pushd Ipopt
./configure --with-mumps
make -j14
make install
popd
rm -rf Ipopt
ln -s /usr/local/include/coin-or /usr/local/include/coin

# Install pybind11
git clone https://github.com/pybind/pybind11.git
pushd pybind11
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf pybind11

# Install FCL
# Key note: this needs to happen before octomap
git clone https://github.com/flexible-collision-library/fcl.git
pushd fcl
git checkout 0.3.4
# vi include/fcl/narrowphase/detail/convexity_based_algorithm/gjk_libccd-inl.h:1696 # "std::max(1.0, v0_dist)" -> "std::max(1.0, (double)v0_dist)"
sed -i '1696s/v0_dist/(double)v0_dist/' include/fcl/narrowphase/detail/convexity_based_algorithm/gjk_libccd-inl.h
mkdir build
pushd build
cmake .. -DFCL_WITH_OCTOMAP=OFF
make install -j14
popd
popd
rm -rf fcl

# Install octomap
git clone https://github.com/OctoMap/octomap.git
git checkout v1.8.1
pushd octomap
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf octomap

# Install tinyxml2
git clone https://github.com/gonultasbu/tinyxml2.git
pushd tinyxml2
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf tinyxml2

# Install freeglut
curl https://managedway.dl.sourceforge.net/project/freeglut/freeglut/3.2.1/freeglut-3.2.1.tar.gz > freeglut.tar.gz
tar -zxf freeglut.tar.gz
rm freeglut.tar.gz
pushd freeglut-3.2.1
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf freeglut-3.2.1

# Install Open Scene Graph
git clone https://github.com/openscenegraph/OpenSceneGraph.git
pushd OpenSceneGraph
git checkout OpenSceneGraph-3.6.5
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf OpenSceneGraph

# Install tinyxml1
git clone https://github.com/robotology-dependencies/tinyxml.git
pushd tinyxml
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf tinyxml

# Install urdfdom_headers
git clone https://github.com/ros/urdfdom_headers.git
pushd urdfdom_headers
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf urdfdom_headers

# Install console_bridge
git clone https://github.com/ros/console_bridge.git
pushd console_bridge
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf console_bridge

# Install urdfdom
git clone https://github.com/ros/urdfdom.git
pushd urdfdom
mkdir build
pushd build
cmake ..
make install -j10
popd
popd
rm -rf urdfdom

# Install perfutils
git clone https://github.com/PlatformLab/PerfUtils.git
pushd PerfUtils
mkdir build
pushd build
cmake ..
make install
popd
popd
rm -rf PerfUtils

# Install Protobuf
PROTOBUF_VERSION="3.14.0"
wget https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/protobuf-all-${PROTOBUF_VERSION}.tar.gz
tar -xvzf protobuf-all-${PROTOBUF_VERSION}.tar.gz
rm protobuf-all-${PROTOBUF_VERSION}.tar.gz
pushd protobuf-${PROTOBUF_VERSION}
./configure
make -j16
make check -j16
make install
popd
rm -rf protobuf-${PROTOBUF_VERSION}

# Install GRPC
git clone --recurse-submodules -b v1.33.2 https://github.com/grpc/grpc
pushd grpc
mkdir -p cmake/build
pushd cmake/build
cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      ../..
make -j
make install
popd
popd
rm -rf grpc

# Install Google benchmark
git clone https://github.com/google/benchmark.git
git clone https://github.com/google/googletest.git benchmark/googletest
pushd benchmark
mkdir build
pushd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make install
popd
popd
rm -rf benchmark

# Install MPFR - Arbitrary precision floating point
sudo apt-get install -y libgmp3-dev # brew install gmp
curl https://www.mpfr.org/mpfr-current/mpfr-4.1.0.tar.gz > mpfr-4.1.0.tar.gz
tar -zxf mpfr-4.1.0.tar.gz
pushd mpfr-4.1.0
./configure
make
sudo make install
sudo ldconfig
popd
rm -rf mpfr-4.1.0
rm mpfr-4.1.0.tar.gz

# Install MPIR - Arbitrary precision integer math
sudo apt-get install -y yasm m4 #brew install yasm m4
curl http://mpir.org/mpir-3.0.0.tar.bz2 > mpir-3.0.0.tar.bz2
tar -xf mpir-3.0.0.tar.bz2
pushd mpir-3.0.0
./configure --enable-cxx
make
sudo make install
sudo ldconfig
popd
rm -rf mpir-3.0.0
rm mpir-3.0.0.tar.bz2

# Install MPFRC++
wget https://github.com/advanpix/mpreal/archive/refs/tags/mpfrc++-3.6.8.tar.gz
tar -xzf mpfrc++-3.6.8.tar.gz
pushd mpreal-mpfrc-3.6.8
sudo cp mpreal.h /usr/include/
popd
rm -rf mpreal-mpfrc-3.6.8
rm mpfrc++-3.6.8.tar.gz