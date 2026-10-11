# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_WS9652A_REGISTER_BUS ws9652a_bus.c)
zephyr_library_sources_ifdef(CONFIG_WS9652A_PMU_BRIDGE ws9652a_pmu_bridge.c)
