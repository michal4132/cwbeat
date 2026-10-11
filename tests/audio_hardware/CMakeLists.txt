# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cwbeat_audio_hardware)
target_sources(app PRIVATE src/main.c ../../subsys/audio/hardware.c)
target_include_directories(app PRIVATE ../../subsys/audio)
