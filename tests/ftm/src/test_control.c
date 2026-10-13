/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The control: this case really does drive the FTM.
 *
 * Compiled into every layer. If this fails, no verdict from the layer under test
 * means anything.
 *
 * Two claims: that the counter counts once a waveform has been asked for, and that
 * the request leaves a register image distinguishable from one at reset. Every
 * later layer tells "restored" from "came back from reset" by comparing against
 * that image.
 */

#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include "ftm.h"

ZTEST(ftm_pm, test_control_drives_the_pwm)
{
	FTM_FOREACH_CASE(c) {
		struct ftm_image running;
		bool held = false;
		int set_rc;
		bool advanced;
		bool clocked;

		/*
		 * Under runtime PM the device boots suspended and this driver takes no
		 * reference of its own, so the control has to ask for it. That an
		 * unwrapped call is refused is test_runtime.c's claim, not a premise of
		 * the control.
		 */
		if (pm_device_runtime_is_enabled(c->dev)) {
			zassert_ok(pm_device_runtime_get(c->dev),
				   "%s: could not resume the device for the control",
				   c->dev->name);
			held = true;
		}

		/*
		 * Every register read is taken while the reference is still held. The
		 * put() below gates the peripheral clock, and reading a gated FTM register
		 * on these parts is a bus fault rather than a stale value.
		 */
		set_rc = ftm_set_pwm(c);
		advanced = ftm_advances(c);
		clocked = ftm_running(c);
		ftm_image_read(c, &running);
		ftm_report_hw(c, "control");

		if (held) {
			/* Before the assertions: a failure must not leak the reference. */
			(void)pm_device_runtime_put(c->dev);
		}

		zassert_ok(set_rc, "%s: pwm_set_cycles() failed (%d)", c->dev->name, set_rc);
		zassert_true(advanced, "%s: CNT did not advance with a waveform set",
			     c->dev->name);

		/*
		 * MOD and CnV are the only register state in this peripheral that a restore
		 * can be seen to put back: both read zero out of reset, so an image
		 * where they are still zero makes every later comparison a
		 * zero-against-zero.
		 */
		zassert_not_equal(running.mod, 0U, "%s: MOD is 0 with a waveform set",
				  c->dev->name);
		zassert_not_equal(running.cnv, 0U, "%s: CnV%u is 0 with a waveform set",
				  c->dev->name, (unsigned int)FTM_PWM_CHANNEL);
		zassert_true(clocked, "%s: SC[CLKS] is clear with a waveform set",
			     c->dev->name);
	}
}
