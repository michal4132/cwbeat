# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()
zephyr_library_sources_ifdef(CONFIG_CHARGER_SC89619D charger_sc89619d.c)
zephyr_library_sources_ifdef(CONFIG_CHARGER_SHELL charger_shell.c)
