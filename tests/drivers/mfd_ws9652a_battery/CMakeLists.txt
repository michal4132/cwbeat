# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(adc_ws9652a)
option(TEST_BATTERY_HELPER "Exercise the CWbeat seven-sample helper" ON)
target_compile_definitions(app PRIVATE CONFIG_MFD_WS9652A_RUNTIME_READ=1
  CONFIG_MFD_WS9652A_LOG_LEVEL=4 CONFIG_ADC_WS9652A_LOG_LEVEL=4)
if(TEST_BATTERY_HELPER)
  target_compile_definitions(app PRIVATE TEST_BATTERY_HELPER=1)
endif()
target_sources(app PRIVATE src/main.c src/pmu.c)
