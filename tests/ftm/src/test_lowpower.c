/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The sysmanaged layer: the device sweep across one transition.
 *
 * One layer, not two: no SoC with an FTM declares a suspend-to-ram state, so there
 * is no dpd build for this file to serve as well.
 *
 * That makes this layer carry the claim dpd would have carried on a part that has
 * one. The driver gates the peripheral clock from SUSPEND -- FTM_Deinit() halts the
 * counter and then gates it -- and brings the peripheral up from scratch in RESUME,
 * FTM_Init() and all. So the register image is unreachable for the whole of the
 * transition whether or not the silicon cut power, and the sweep RESUME has to
 * rebuild the waveform from the image the driver kept in RAM rather than from
 * anything the hardware remembered. A driver with no restore path fails here.
 *
 * What is asserted after the transition is behavioural as well as register-level:
 * that the counter counts again. An FTM takes its counter clock through a source
 * and prescaler the board devicetree picks rather than this driver, so a register
 * image that came back perfectly while the clock source did not would look
 * restored and still not count.
 */

#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include <pm_test/lowpower.h>
#include <pm_test/state.h>

#include "ftm.h"

ZTEST(ftm_pm, test_sweep_across_transition)
{
	FTM_FOREACH_CASE(c) {
		struct ftm_image running;
		struct ftm_image after;
		bool gate_after;

		/* No runtime PM in this layer, so the sweep is the only thing that will
		 * suspend the device.
		 */
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		zassert_ok(ftm_start_and_verify(c),
			   "%s: the counter did not run before the transition", c->dev->name);
		ftm_image_read(c, &running);
		ftm_report_hw(c, "before");

		pm_test_enter_transition();

		/* Sampled before the console is revived: reviving it takes time, and the
		 * question is what the peripheral looked like the moment the sweep handed it
		 * back.
		 */
		gate_after = ftm_gate_is_open(c);
		if (gate_after) {
			ftm_image_read(c, &after);
		}

		pm_test_console_resume(PM_TEST_CONSOLE_REINIT_PINMUX);
		TC_PRINT("resumed from %s\n", PM_TEST_TRANSITION_NAME);
		ftm_report_hw(c, "after-resume");

		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		if (!ftm_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, register claims skipped\n",
				 c->dev->name);
		} else {
			/*
			 * The gate first: without it the register comparison below would be
			 * reading an unclocked peripheral, and restored would be whatever the bus
			 * returns.
			 */
			zassert_true(gate_after,
				     "%s: the clock gate was not re-opened across the transition",
				     c->dev->name);
			zassert_true(ftm_image_equal(&after, &running),
				     "%s: the waveform did not survive the transition: "
				     "SC=0x%08x MOD=0x%08x CnV=0x%08x, want 0x%08x/0x%08x/0x%08x",
				     c->dev->name, after.sc, after.mod, after.cnv, running.sc,
				     running.mod, running.cnv);
		}

		/* And the peripheral has to work, not just read back right. */
		zassert_true(ftm_advances(c),
			     "%s: the counter is not counting after the transition",
			     c->dev->name);
	}
}
