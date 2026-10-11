/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The control: this case really does drive the CTIMER.
 *
 * Compiled into every layer. If this fails, no verdict from the layer under test
 * means anything.
 *
 * Two claims: that the counter counts, and that an armed alarm leaves a register
 * image distinguishable from a reset block. Every later layer tells "restored" from
 * "came back from reset" by comparing against that image.
 */

#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include "ctimer.h"

ZTEST(ctimer_pm, test_control_counts_and_arms)
{
	CTIMER_FOREACH_CASE(c) {
		struct ctimer_image armed;
		bool held = false;
		int start_rc;
		int arm_rc;
		bool fired;

		/*
		 * Under runtime PM the device boots suspended and this driver takes no
		 * reference of its own, so the control has to ask for it. That an
		 * unwrapped call is refused is test_runtime.c's claim, not a premise
		 * of the control.
		 */
		if (pm_device_runtime_is_enabled(c->dev)) {
			zassert_ok(pm_device_runtime_get(c->dev),
				   "%s: could not resume the device for the control",
				   c->dev->name);
			held = true;
		}

		start_rc = ctimer_start_and_verify(c);
		arm_rc = ctimer_arm_alarm(c, 20U);
		ctimer_image_read(c, &armed);
		fired = ctimer_alarm_wait(c, 500U);

		(void)ctimer_cancel_alarm(c);
		(void)ctimer_stop(c);
		ctimer_report_hw(c, "control");

		if (held) {
			/* Before the assertions: a failure must not leak the reference. */
			(void)pm_device_runtime_put(c->dev);
		}

		zassert_ok(start_rc, "%s: counter did not start and count (%d)", c->dev->name,
			   start_rc);
		zassert_ok(arm_rc, "%s: counter_set_alarm() failed (%d)", c->dev->name, arm_rc);
		zassert_true(fired, "%s: the alarm never fired", c->dev->name);

		/*
		 * The prescale, mode and input this driver writes from devicetree are all
		 * 0 on every board here, which is also what a reset leaves -- so the
		 * armed alarm is the only register state in this block that a restore can
		 * be seen to put back.
		 */
		zassert_not_equal(armed.mr, 0U, "%s: MR%u is 0 with an alarm armed",
				  c->dev->name, (unsigned int)CTIMER_ALARM_CHANNEL);
		zassert_not_equal(armed.mcr, 0U, "%s: MCR is 0 with an alarm armed",
				  c->dev->name);
	}
}
