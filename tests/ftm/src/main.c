/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief FTM device-PM suite: the suite hooks, and nothing else.
 *
 * Which layer this build tests, and therefore which src/test_*.c is compiled in,
 * is decided by the pm-* snippet on the command line. See docs/pm-layers.md.
 */

#include <zephyr/ztest.h>

#include <pm_test/layer.h>
#include <pm_test/state.h>

#include "ftm.h"

static void *ftm_pm_setup(void)
{
	(void)pm_test_setup();
	ftm_report_configuration();

	FTM_FOREACH_CASE(c) {
		pm_test_note_state(c->dev, "at boot");
		ftm_report_hw(c, "at boot");
	}

	return NULL;
}

static void ftm_pm_before(void *fixture)
{
	ARG_UNUSED(fixture);

	FTM_FOREACH_CASE(c) {
		zassert_true(device_is_ready(c->dev), "%s is not ready", c->dev->name);
	}
}

ZTEST_SUITE(ftm_pm, NULL, ftm_pm_setup, ftm_pm_before, NULL, NULL);
