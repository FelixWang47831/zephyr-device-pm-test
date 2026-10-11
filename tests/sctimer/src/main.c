/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief SCTimer device-PM suite: the suite hooks, and nothing else.
 *
 * Which layer this build tests, and therefore which src/test_*.c is compiled in,
 * is decided by the pm-* snippet on the command line. See docs/pm-layers.md.
 */

#include <zephyr/ztest.h>

#include <pm_test/layer.h>
#include <pm_test/state.h>

#include "sctimer.h"

static void *sctimer_pm_setup(void)
{
	(void)pm_test_setup();
	sctimer_report_configuration();

	SCTIMER_FOREACH_CASE(c) {
		pm_test_note_state(c->dev, "at boot");
		sctimer_report_hw(c, "at boot");
	}

	return NULL;
}

static void sctimer_pm_before(void *fixture)
{
	ARG_UNUSED(fixture);

	SCTIMER_FOREACH_CASE(c) {
		zassert_true(device_is_ready(c->dev), "%s is not ready", c->dev->name);
	}
}

ZTEST_SUITE(sctimer_pm, NULL, sctimer_pm_setup, sctimer_pm_before, NULL, NULL);
