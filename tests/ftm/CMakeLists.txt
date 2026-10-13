# Copyright 2026 NXP
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Pulls in this repository as a Zephyr module, which is what makes lib/pm_test,
# include/pm_test and the snippets/pm-* snippets available. Nothing has to be
# passed on the command line: `-S pm-<layer>` is the whole interface.
list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_LIST_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ftm_device_pm)

# The suite, the register view, and the control test that every layer runs.
target_sources(app PRIVATE src/main.c src/ftm.c src/test_control.c)

# One file per layer. A layer file is the only place that layer claims live, so a
# build for a layer contains no code belonging to any other.
#
# There is no dpd scenario: no SoC with an FTM declares a suspend-to-ram power
# state, so pm-dpd would not even configure. See README.md.
target_sources_ifdef(CONFIG_PM_TEST_LAYER_DEVICE     app PRIVATE src/test_device.c)
target_sources_ifdef(CONFIG_PM_TEST_LAYER_RUNTIME    app PRIVATE src/test_runtime.c)
target_sources_ifdef(CONFIG_PM_TEST_LAYER_SYSTEM     app PRIVATE src/test_system.c)
target_sources_ifdef(CONFIG_PM_TEST_LAYER_SYSMANAGED app PRIVATE src/test_lowpower.c)
