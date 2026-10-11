# Copyright 2026 Michał Bogdziewicz michal@bogdziewicz.xyz
# SPDX-License-Identifier: Apache-2.0

# This file contains helper functions that reduce the amount of code
# required to add app sources and include directories.
# Based on helpers for the zephyr target.

# Based on zephyr/cmake/extensions.cmake:zephyr_sources().
function(app_sources)
  foreach(arg ${ARGV})
    if(IS_DIRECTORY ${arg})
      message(FATAL_ERROR "app_sources() was called on a directory")
    endif()
    target_sources(app PRIVATE ${arg})
  endforeach()
endfunction()

# Based on zephyr/cmake/extensions.cmake:zephyr_sources_ifdef().
function(app_sources_ifdef feature_toggle)
  if(${${feature_toggle}})
    app_sources(${ARGN})
  endif()
endfunction()

# Based on zephyr/cmake/extensions.cmake:zephyr_include_directories().
# Use `app PRIVATE` instead of `app_interface INTERFACE`
# as the app should have no dependents and thus no interface.
function(app_include_directories)
  target_include_directories(app PRIVATE ${ARGV})
endfunction()
