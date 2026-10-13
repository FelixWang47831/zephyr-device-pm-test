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
