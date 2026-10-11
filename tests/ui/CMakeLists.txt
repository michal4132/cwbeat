# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cwbeat_ui)
target_sources(app PRIVATE src/main.c ../../subsys/ui/ui.c ../../subsys/ui/gestures.c)
target_include_directories(app PRIVATE ../../subsys/ui)
