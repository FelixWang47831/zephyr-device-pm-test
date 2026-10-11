/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The sysmanaged and dpd layers: the device sweep across one transition.
 *
 * One file for two layers, because the sequence is identical and only the state
 * being entered differs. In sysmanaged the sweep's SUSPEND and RESUME are all that
 * happen, so the block keeps its power: re-opening the clock gate hands back the
 * alarm the consumer armed, and it fires on its own. In dpd the CORE domain is power
 * gated: the whole register block comes back at its reset value, the SYSCON clock
 * gate with it, and nothing re-runs driver init -- so TURN_ON has to rebuild the
 * block from the devicetree and re-connect the interrupt, while the run the consumer
 * had is gone and arming again is theirs.
 *
 * What is asserted after the transition is behavioural as well as register-level:
 * that the counter counts and that an alarm fires -- the one the consumer armed
 * where power was kept, a freshly armed one where it was not. A CTIMER's functional
 * clock is selected by the board rather than by this driver, so a register image
 * that came back perfectly while the clock mux did not would look restored and still
 * not count.
 */

#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include <pm_test/lowpower.h>
#include <pm_test/state.h>

#include "ctimer.h"

ZTEST(ctimer_pm, test_sweep_across_transition)
{
	CTIMER_FOREACH_CASE(c) {
		struct ctimer_image armed;
		struct ctimer_image after;
		bool gate_after;

		/* No runtime PM in these layers, so the sweep is the only thing that will
		 * suspend the device.
		 */
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		zassert_ok(ctimer_start_and_verify(c), "%s: counter did not start before the "
			   "transition", c->dev->name);

		/*
		 * Far enough out that the console traffic below cannot consume it before
		 * the transition, and the counter is stopped for the whole of the
		 * transition, so what is left of it is measured from the resume.
		 */
		zassert_ok(ctimer_arm_alarm(c, 200U), "%s: could not arm the alarm",
			   c->dev->name);
		ctimer_image_read(c, &armed);
		ctimer_report_hw(c, "before");

		pm_test_enter_transition();

		/* Sampled before the console is revived: reviving it takes time, and the
		 * question is what the block looked like the moment the sweep handed it
		 * back.
		 */
		gate_after = ctimer_gate_is_open(c);
		if (gate_after) {
			ctimer_image_read(c, &after);
		}

		pm_test_console_resume(PM_TEST_CONSOLE_REINIT_PINMUX);
		TC_PRINT("resumed from %s\n", PM_TEST_TRANSITION_NAME);
		ctimer_report_hw(c, "after-resume");

		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		if (!ctimer_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, register claims skipped\n",
				 c->dev->name);
		} else {
			/*
			 * The gate first: without it the register comparison below would be
			 * reading an unclocked block, and "restored" would be whatever the
			 * bus returns.
			 */
			zassert_true(gate_after,
				     "%s: the clock gate was not re-opened across the "
				     "transition", c->dev->name);
#if defined(CONFIG_PM_TEST_LAYER_DPD)
			zassert_false(ctimer_image_equal(&after, &armed),
				      "%s: the armed alarm came back across %s, so the block "
				      "kept its power", c->dev->name, PM_TEST_TRANSITION_NAME);
#else
			zassert_true(ctimer_image_equal(&after, &armed),
				     "%s: the armed alarm did not survive the transition: "
				     "TCR=0x%08x MCR=0x%08x MR=0x%08x, want 0x%08x/0x%08x/"
				     "0x%08x", c->dev->name, after.tcr, after.mcr, after.mr,
				     armed.tcr, armed.mcr, armed.mr);
#endif
		}

#if defined(CONFIG_PM_TEST_LAYER_DPD)
		/*
		 * Power was cut, so the counter is back at reset and nothing is armed.
		 * Starting and arming from scratch cannot succeed unless TURN_ON rebuilt
		 * the block and re-connected the interrupt.
		 */
		zassert_false(ctimer_running(c),
			      "%s: the counter is running after %s without being started",
			      c->dev->name, PM_TEST_TRANSITION_NAME);
		zassert_ok(ctimer_start_and_verify(c),
			   "%s: the counter does not start after %s", c->dev->name,
			   PM_TEST_TRANSITION_NAME);
		zassert_ok(ctimer_arm_alarm(c, 200U),
			   "%s: the alarm cannot be armed after %s", c->dev->name,
			   PM_TEST_TRANSITION_NAME);
		zassert_true(ctimer_alarm_wait(c, 1000U),
			     "%s: an alarm armed after %s did not fire", c->dev->name,
			     PM_TEST_TRANSITION_NAME);
#else
		/* And the block has to work, not just read back right. */
		zassert_true(ctimer_advances(c),
			     "%s: the counter is not counting after the transition",
			     c->dev->name);
		zassert_true(ctimer_alarm_wait(c, 1000U),
			     "%s: the alarm the consumer armed did not fire after the "
			     "transition", c->dev->name);
#endif

		(void)ctimer_cancel_alarm(c);
		(void)ctimer_stop(c);
	}
}
