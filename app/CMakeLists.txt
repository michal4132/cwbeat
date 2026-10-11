# Copyright 2026 Michał Bogdziewicz michal@bogdziewicz.xyz
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.25)

get_filename_component(MODULE_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH ${MODULE_ROOT}/cmake)

include(app_target)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(cwbeat)

add_subdirectory(src)