# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mfd_ws9652a_transports)
option(TEST_NATIVE_I2C "Exercise WS9652A detailed I2C completion reporting" ON)
target_compile_definitions(app PRIVATE CONFIG_MFD_WS9652A_RUNTIME_READ=1
  CONFIG_MFD_WS9652A_LOG_LEVEL=4
  CONFIG_WS9652A_PMU_BRIDGE=1)
if(TEST_NATIVE_I2C)
  target_compile_definitions(app PRIVATE CONFIG_I2C_WS9652A=1)
endif()
target_include_directories(app PRIVATE ../../../drivers/mfd/ws9652a)
target_sources(app PRIVATE src/main.c
  ../../../drivers/mfd/ws9652a/pmu_transport.c
  ../../../drivers/mfd/ws9652a/go4_internal_bus.c
  ../../../drivers/mfd/ws9652a/go4_internal_bootstrap.c
  ../../../drivers/mfd/ws9652a/power_policy.c)
