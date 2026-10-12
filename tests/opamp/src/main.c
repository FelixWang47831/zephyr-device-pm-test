/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief OPAMP device-PM suite: the suite hooks, and nothing else.
 *
 * Which layer this build tests, and therefore which src/test_*.c is compiled in,
 * is decided by the pm-* snippet on the command line. See docs/pm-layers.md.
 */

#include <zephyr/ztest.h>

#include <pm_test/layer.h>
#include <pm_test/state.h>

#include "opamp.h"

static void *opamp_pm_setup(void)
{
	(void)pm_test_setup();
	pm_test_note_state(OPAMP_DEV, "at boot");
	opamp_report_hw("at boot");
	return NULL;
}

static void opamp_pm_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_true(device_is_ready(OPAMP_DEV), "%s is not ready", OPAMP_DEV->name);
}

ZTEST_SUITE(opamp_pm, NULL, opamp_pm_setup, opamp_pm_before, NULL, NULL);
