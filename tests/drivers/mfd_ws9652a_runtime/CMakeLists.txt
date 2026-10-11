# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mfd_ws9652a_runtime)
# Include the production wrapper with a synthetic device and mocked transport.
# A real PMU DT node would also link the physical bus implementation on QEMU.
option(TEST_RUNTIME_READ "Exercise the opt-in runtime API" ON)
if(TEST_RUNTIME_READ)
  target_compile_definitions(app PRIVATE CONFIG_MFD_WS9652A_RUNTIME_READ=1)
endif()
target_compile_definitions(app PRIVATE CONFIG_MFD_WS9652A_LOG_LEVEL=4)
target_sources(app PRIVATE src/main.c)
