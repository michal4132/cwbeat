# SPDX-License-Identifier: Apache-2.0
zephyr_library_amend()
zephyr_library_sources_ifdef(CONFIG_CLOCK_CONTROL_WS9652A clock_control_ws9652a.c)
