/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The device layer: the driver's PM callbacks, driven directly.
 *
 * Two tests, for the two halves of the callback. The first is the SUSPEND/RESUME
 * round trip, which owns the clock gate and the park. The second is TURN_OFF/TURN_ON,
 * which owns the register block -- and since nothing in this layer really cut power
 * to it, the block is clobbered by hand so that the replay has something to put back.
 * What a real power cycle does is asked in src/test_lowpower.c under the dpd layer.
 *
 * Note which action is expected to bring the block up. TURN_ON rebuilds the register
 * block and replays the channel the consumer had configured, borrowing the clock to
 * do it and handing it back before it returns, so an instance which boots SUSPENDED
 * under runtime PM boots configured but genuinely gated and not driving. RESUME
 * re-opens the gate and restarts the counter. The tests assert that split: a gated
 * SCT register does not answer, so the block is read by borrowing the clock back --
 * see sctimer_peek_gated() -- after the gate itself has been sampled.
 *
 * What a suspend owes the consumer: the channel programming stays in the registers,
 * the counter is halted, and the output is held at its inactive level rather than
 * frozen wherever the counter happened to stop. A resume restarts the counter the
 * consumer was running, without the consumer asking again -- see src/sctimer.h for
 * the contract.
 */

#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include <pm_test/state.h>

#include "sctimer.h"

ZTEST(sctimer_pm, test_device_suspend_resume)
{
	SCTIMER_FOREACH_CASE(c) {
		struct sctimer_image configured;
		struct sctimer_image after;
		struct sctimer_peek parked;
		bool gate_after_suspend;
		int err;

		if (!sctimer_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		/* No runtime PM in this layer, so pm_device_driver_init() resumed it. */
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_true(sctimer_gate_is_open(c), "%s: gate closed while PM reports ACTIVE",
			     c->dev->name);

		zassert_ok(sctimer_configure(c), "%s: pwm_set_cycles() failed", c->dev->name);
		sctimer_image_read(c, &configured);
		zassert_true(sctimer_output_toggles(c), "%s: the output is not toggling before "
			     "the suspend", c->dev->name);
		sctimer_report_hw(c, "before-suspend");

		zassert_ok(pm_device_action_run(c->dev, PM_DEVICE_ACTION_SUSPEND),
			   "%s: SUSPEND action rejected", c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_SUSPENDED);

		/*
		 * The gate is sampled first and on its own: an unclocked block does not
		 * answer, and reading it to find out whether it is clocked is the one
		 * thing that cannot be done. Only then is the clock borrowed back to see
		 * what the suspend left in the registers.
		 */
		gate_after_suspend = sctimer_gate_is_open(c);
		sctimer_peek_gated(c, &parked);
		sctimer_report_hw(c, "after-suspend");

		zassert_false(gate_after_suspend, "%s: clock gate still open after SUSPEND",
			      c->dev->name);

		/* The park: halted, held inactive, and still programmed. */
		zassert_false(parked.running, "%s: the counter is still running after SUSPEND",
			      c->dev->name);
		zassert_false(parked.output_high,
			      "%s: SUSPEND left the output high, not at its inactive level",
			      c->dev->name);
		zassert_true(sctimer_image_equal(&parked.image, &configured),
			     "%s: SUSPEND destroyed the channel configuration: MATCHREL0=0x%08x "
			     "MATCHREL1=0x%08x SET=0x%08x CLR=0x%08x, want 0x%08x/0x%08x/0x%08x/"
			     "0x%08x", c->dev->name, parked.image.matchrel_period,
			     parked.image.matchrel_pulse, parked.image.out_set,
			     parked.image.out_clr, configured.matchrel_period,
			     configured.matchrel_pulse, configured.out_set, configured.out_clr);

		zassert_ok(pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME),
			   "%s: RESUME action rejected", c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		sctimer_image_read(c, &after);
		sctimer_report_hw(c, "after-resume");

		zassert_true(sctimer_gate_is_open(c), "%s: gate still closed after RESUME",
			     c->dev->name);

		/*
		 * The channel the consumer configured is driving again, and the consumer
		 * did nothing to make that happen.
		 */
		zassert_true(sctimer_image_equal(&after, &configured),
			     "%s: RESUME left MATCHREL0=0x%08x MATCHREL1=0x%08x SET=0x%08x "
			     "CLR=0x%08x, want 0x%08x/0x%08x/0x%08x/0x%08x", c->dev->name,
			     after.matchrel_period, after.matchrel_pulse, after.out_set,
			     after.out_clr, configured.matchrel_period, configured.matchrel_pulse,
			     configured.out_set, configured.out_clr);
		zassert_true(sctimer_advances(c), "%s: the counter is not running after RESUME",
			     c->dev->name);
		zassert_true(sctimer_output_toggles(c), "%s: the output is not toggling after "
			     "RESUME", c->dev->name);

		/* A redundant action must be refused, not obeyed and not fatal. */
		err = pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME);
		zassert_equal(err, -EALREADY, "%s: second RESUME returned %d, want -EALREADY",
			      c->dev->name, err);
	}
}

ZTEST(sctimer_pm, test_device_turn_on_replays_the_channel)
{
	SCTIMER_FOREACH_CASE(c) {
		struct sctimer_image configured;
		struct sctimer_image clobbered;
		struct sctimer_image after_resume;
		struct sctimer_peek after_turn_on;
		bool gate_after_turn_on;
		int suspend_rc;
		int turn_off_rc;
		int turn_on_rc;
		int resume_rc;

		if (!sctimer_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_ok(sctimer_configure(c), "%s: pwm_set_cycles() failed", c->dev->name);
		sctimer_image_read(c, &configured);

		/*
		 * Collected in one go and asserted afterwards, so that a failure cannot
		 * hand the next test a device the core still records as OFF.
		 */
		suspend_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_SUSPEND);

		sctimer_clobber(c, &clobbered);

		turn_off_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_TURN_OFF);

		turn_on_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_TURN_ON);
		gate_after_turn_on = sctimer_gate_is_open(c);
		sctimer_peek_gated(c, &after_turn_on);

		resume_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME);
		sctimer_image_read(c, &after_resume);
		sctimer_report_hw(c, "after-resume");

		zassert_ok(suspend_rc, "%s: SUSPEND rejected (%d)", c->dev->name, suspend_rc);
		zassert_ok(turn_off_rc, "%s: TURN_OFF rejected (%d)", c->dev->name, turn_off_rc);
		zassert_ok(turn_on_rc, "%s: TURN_ON rejected (%d)", c->dev->name, turn_on_rc);
		zassert_ok(resume_rc, "%s: RESUME rejected (%d)", c->dev->name, resume_rc);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		/* Before anything else: a clobber that did not take would make the claims
		 * below answerable without the block ever having been disturbed.
		 */
		zassert_true(sctimer_image_is_clear(&clobbered), "%s: the clobber did not take",
			     c->dev->name);

		/*
		 * TURN_ON returns with the core about to record the device as suspended,
		 * so a block left clocked, or left counting, would be a block running
		 * behind a PM state that says it is not.
		 */
		zassert_false(gate_after_turn_on, "%s: TURN_ON opened the clock gate",
			      c->dev->name);
		zassert_false(after_turn_on.running, "%s: TURN_ON left the counter running",
			      c->dev->name);
		zassert_false(after_turn_on.output_high,
			      "%s: TURN_ON left the output high, not at its inactive level",
			      c->dev->name);

		/*
		 * And the replay is TURN_ON's, not RESUME's: what the consumer configured
		 * is back in the registers before the device is resumed, because a power
		 * domain cycle is the case this exists for and nothing re-runs init there.
		 */
		zassert_true(sctimer_image_equal(&after_turn_on.image, &configured),
			     "%s: TURN_ON did not replay the channel: MATCHREL0=0x%08x "
			     "MATCHREL1=0x%08x SET=0x%08x CLR=0x%08x, want 0x%08x/0x%08x/"
			     "0x%08x/0x%08x", c->dev->name, after_turn_on.image.matchrel_period,
			     after_turn_on.image.matchrel_pulse, after_turn_on.image.out_set,
			     after_turn_on.image.out_clr, configured.matchrel_period,
			     configured.matchrel_pulse, configured.out_set, configured.out_clr);

		zassert_true(sctimer_gate_is_open(c), "%s: RESUME left the clock gate closed",
			     c->dev->name);
		zassert_true(sctimer_image_equal(&after_resume, &configured),
			     "%s: the round trip did not restore the channel", c->dev->name);

		/* And the block has to drive, not just read back right. */
		zassert_true(sctimer_advances(c),
			     "%s: the counter is not running after the round trip",
			     c->dev->name);
		zassert_true(sctimer_output_toggles(c),
			     "%s: the output is not toggling after the round trip",
			     c->dev->name);

		/* Reconfiguring still works on the rebuilt block. */
		zassert_ok(sctimer_configure(c),
			   "%s: the channel cannot be reconfigured after the round trip",
			   c->dev->name);
	}
}
