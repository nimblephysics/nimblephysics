![Stanford Nimble Logo](https://nimblephysics.org/README/README_Splash.svg)

[![Tests](https://github.com/nimblephysics/nimblephysics/actions/workflows/ci_docker.yml/badge.svg)](https://github.com/nimblephysics/nimblephysics/actions/workflows/ci_docker.yml)

# Stanford Nimble


### Update ! Since the last package contains issues, use this version 
`pip3 install nimblephysics==0.10.52.1`

** BETA SOFTWARE **

[Read our docs](http://www.nimblephysics.org/docs) and [the paper](https://arxiv.org/abs/2103.16021).

Use physics as a non-linearity in your neural network. A single timestep, `nimble.timestep(state, controls)`, is a valid PyTorch function.

![Forward pass illustration](https://nimblephysics.org/README/README_DataFlow_Fwd.svg)

We support an analytical backwards pass, that works even through contact and friction.

![Backpropagation illustration](https://nimblephysics.org/README/README_DataFlow_Back.svg)

It's as easy as:

```python
from nimble import timestep

# Everything is a PyTorch Tensor, and this is differentiable!!
next_state = timestep(world, current_state, control_forces)
```

Nimble started life as a fork of the popular DART physics engine, with analytical gradients and a PyTorch binding. We've worked hard to maintain as much backwards compatability as we can, so many simulations that worked in DART should translate directly to Nimble.

Check out our [website](http://www.nimblephysics.org) for more information.

### Installing on Arm64 Macs (M1, M2, etc)

We don't yet publish Arm64 binaries to PyPI from our CI system, so you may not be able to `pip3 install nimblephysics` from a new Arm64 Mac.
We will endeavor to manually push binaries occassionally, but until GitHub Actions supports using Arm64 Mac runners, that may run a bit behind.

Currently, the pre-built Arm64 binaries are ONLY AVAILABLE ON PYTHON 3.9. So if you create a virtual environment with Python 3.9, and then `pip3 install nimblephysics`, that should work.

If you really need another Python version for some reason, the solution is to clone this repo, then run
- `ci/mac/install_dependencies.sh`
- `ci/mac/manually_build_arm64_wheels.sh`
That will install the dependencies you need, and then build and install the Python package. Please create Issues if you run into problems, and we'll do our best to fix them.
