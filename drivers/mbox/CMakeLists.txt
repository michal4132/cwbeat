# SPDX-License-Identifier: Apache-2.0
zephyr_library_amend()
zephyr_library_sources_ifdef(CONFIG_MBOX_WS9652A mbox_ws9652a.c)
