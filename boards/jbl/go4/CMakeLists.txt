# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_SOC_WS9652A_CPUTOP
  board.c power_off.c recovery.c)
