/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The device layer: the driver's PM callbacks, driven directly.
 *
 * Two tests, for the two halves of the callback. The first is the SUSPEND/RESUME
 * round trip, which owns the clock gate. The second is TURN_OFF/TURN_ON, which owns
 * the register block -- and since nothing in this layer really cut power to it, the
 * block is clobbered by hand so that the restore has something to put back. What a
 * real power cycle does is asked in src/test_lowpower.c under the dpd layer.
 *
 * Note which action is expected to bring the block up. TURN_ON rebuilds the register
 * block, borrowing the function clock and handing it back before it returns, so an
 * instance which boots SUSPENDED under runtime PM boots configured but genuinely
 * gated. RESUME re-opens the gate and restarts a counter that was running. The second
 * test asserts that split as far as it is observable: a CTIMER register answers only
 * while the block is clocked, so the image cannot be read between TURN_ON and RESUME,
 * and what is checked there is that TURN_ON left the gate closed.
 *
 * What each action owes the consumer differs. A suspend keeps the block powered, so
 * the alarm the consumer armed is still there afterwards and fires on its own. A
 * power cycle does not, so what comes back is a stopped counter with nothing armed,
 * and arming again is the consumer's -- see src/ctimer.h for the contract.
 */

#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include <pm_test/state.h>

#include "ctimer.h"

ZTEST(ctimer_pm, test_device_suspend_resume)
{
	CTIMER_FOREACH_CASE(c) {
		struct ctimer_image armed;
		struct ctimer_image after;
		int err;

		if (!ctimer_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		/* No runtime PM in this layer, so pm_device_driver_init() resumed it. */
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_true(ctimer_gate_is_open(c), "%s: gate closed while PM reports ACTIVE",
			     c->dev->name);

		zassert_ok(ctimer_start_and_verify(c), "%s: counter did not start", c->dev->name);
		zassert_ok(ctimer_arm_alarm(c, 50U), "%s: could not arm the alarm", c->dev->name);
		ctimer_image_read(c, &armed);
		ctimer_report_hw(c, "before-suspend");

		zassert_ok(pm_device_action_run(c->dev, PM_DEVICE_ACTION_SUSPEND),
			   "%s: SUSPEND action rejected", c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_SUSPENDED);
		ctimer_report_hw(c, "after-suspend");

		/*
		 * The claim of this layer: the block is no longer clocked. Nothing is read
		 * out of it here -- an unclocked block does not answer, and reading it to
		 * find out whether it is clocked is the one thing that cannot be done.
		 */
		zassert_false(ctimer_gate_is_open(c), "%s: clock gate still open after SUSPEND",
			      c->dev->name);
		zassert_false(ctimer_alarm_fired(c),
			      "%s: the alarm fired while the block was suspended", c->dev->name);

		zassert_ok(pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME),
			   "%s: RESUME action rejected", c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		ctimer_image_read(c, &after);
		ctimer_report_hw(c, "after-resume");

		zassert_true(ctimer_gate_is_open(c), "%s: gate still closed after RESUME",
			     c->dev->name);

		/*
		 * The alarm the consumer armed is still armed, and the counter it was
		 * armed against is running again -- so it fires without the consumer
		 * doing anything.
		 */
		zassert_true(ctimer_image_equal(&after, &armed),
			     "%s: RESUME left TCR=0x%08x MCR=0x%08x MR=0x%08x, want "
			     "0x%08x/0x%08x/0x%08x", c->dev->name, after.tcr, after.mcr,
			     after.mr, armed.tcr, armed.mcr, armed.mr);
		zassert_true(ctimer_advances(c), "%s: the counter is not running after RESUME",
			     c->dev->name);
		zassert_true(ctimer_alarm_wait(c, 500U),
			     "%s: the alarm did not fire after RESUME", c->dev->name);

		/* A redundant action must be refused, not obeyed and not fatal. */
		err = pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME);
		zassert_equal(err, -EALREADY, "%s: second RESUME returned %d, want -EALREADY",
			      c->dev->name, err);

		(void)ctimer_cancel_alarm(c);
		(void)ctimer_stop(c);
	}
}

ZTEST(ctimer_pm, test_device_turn_on_rebuilds_the_register_block)
{
	CTIMER_FOREACH_CASE(c) {
		struct ctimer_image armed;
		struct ctimer_image clobbered;
		struct ctimer_image after_resume;
		bool gate_after_turn_on;
		int suspend_rc;
		int turn_off_rc;
		int turn_on_rc;
		int resume_rc;

		if (!ctimer_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_ok(ctimer_start_and_verify(c), "%s: counter did not start", c->dev->name);
		zassert_ok(ctimer_arm_alarm(c, 50U), "%s: could not arm the alarm", c->dev->name);
		ctimer_image_read(c, &armed);

		/*
		 * Collected in one go and asserted afterwards, so that a failure cannot
		 * hand the next test a device the core still records as OFF.
		 */
		suspend_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_SUSPEND);

		ctimer_clobber(c, &clobbered);

		turn_off_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_TURN_OFF);

		turn_on_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_TURN_ON);
		gate_after_turn_on = ctimer_gate_is_open(c);

		resume_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME);
		ctimer_image_read(c, &after_resume);
		ctimer_report_hw(c, "after-resume");

		zassert_ok(suspend_rc, "%s: SUSPEND rejected (%d)", c->dev->name, suspend_rc);
		zassert_ok(turn_off_rc, "%s: TURN_OFF rejected (%d)", c->dev->name, turn_off_rc);
		zassert_ok(turn_on_rc, "%s: TURN_ON rejected (%d)", c->dev->name, turn_on_rc);
		zassert_ok(resume_rc, "%s: RESUME rejected (%d)", c->dev->name, resume_rc);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		/* Before anything else: a clobber that did not take would make the claims
		 * below answerable without the block ever having been disturbed.
		 */
		zassert_false(ctimer_image_equal(&clobbered, &armed),
			      "%s: the clobber did not take", c->dev->name);

		/*
		 * TURN_ON returns with the core about to record the device as suspended,
		 * so a block left clocked would be a block running behind a PM state that
		 * says it is not.
		 */
		zassert_false(gate_after_turn_on, "%s: TURN_ON opened the clock gate",
			      c->dev->name);
		zassert_true(ctimer_gate_is_open(c), "%s: RESUME left the clock gate closed",
			     c->dev->name);

		/*
		 * The round trip owes a device that can be used again, not the run that
		 * was in progress: the counter comes back stopped with nothing armed,
		 * because neither the count value nor an alarm measured against it
		 * survived the loss of power.
		 */
		zassert_false(ctimer_running(c),
			      "%s: the round trip left the counter running, TCR=0x%08x",
			      c->dev->name, after_resume.tcr);
		zassert_false(ctimer_alarm_fired(c),
			      "%s: an alarm fired after the round trip without being armed",
			      c->dev->name);

		/*
		 * Arming again has to work: an alarm cannot fire unless TURN_ON re-ran
		 * init on the clobbered block and re-connected its interrupt.
		 */
		zassert_ok(ctimer_start_and_verify(c), "%s: the counter does not start after "
			   "the round trip", c->dev->name);
		zassert_ok(ctimer_arm_alarm(c, 50U),
			   "%s: the alarm cannot be armed after the round trip", c->dev->name);
		zassert_true(ctimer_alarm_wait(c, 500U),
			     "%s: an alarm armed after the round trip did not fire",
			     c->dev->name);

		(void)ctimer_cancel_alarm(c);
		(void)ctimer_stop(c);
	}
}
