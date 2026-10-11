# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pmu_bridge_ws9652a)
target_sources(app PRIVATE src/main.c)
# The production sources are included without DT instances or driver Kconfig.
target_compile_definitions(app PRIVATE
  CONFIG_WS9652A_REGISTER_BUS_LOG_LEVEL=4
  CONFIG_WS9652A_PMU_BRIDGE_LOG_LEVEL=4)
