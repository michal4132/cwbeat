# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(peer_ws9652a)
zephyr_include_directories(../../../soc/zgmicro/ws9652a)
target_compile_definitions(app PRIVATE CONFIG_WS9652A_PEER_LOG_LEVEL=4)
target_sources(app PRIVATE src/main.c)
