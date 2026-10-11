# SPDX-License-Identifier: Apache-2.0
zephyr_include_directories(.)
zephyr_sources_ifdef(CONFIG_SOC_WS9652A_CPUTOP soc_cputop.c reset.c)
set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld CACHE INTERNAL "")
