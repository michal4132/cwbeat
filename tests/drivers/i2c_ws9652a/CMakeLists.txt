# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(i2c_ws9652a)
zephyr_include_directories(../../../soc/zgmicro/ws9652a)
target_sources(app PRIVATE src/main.c)
# The fixture includes the controller source without DT driver instances.
# Compile its debug paths although its driver Kconfig is therefore disabled.
target_compile_definitions(app PRIVATE CONFIG_I2C_WS9652A_LOG_LEVEL=4)
