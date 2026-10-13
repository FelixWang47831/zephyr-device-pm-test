/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The device layer: the driver PM callbacks, driven directly.
 *
 * Two tests, for the two halves of the callback. The first is the SUSPEND/RESUME
 * round trip, which owns the clock gate. The second is TURN_OFF/TURN_ON, which
 * owns the register block -- and since nothing in this layer really cut power to
 * it, the registers are clobbered by hand so that the restore has something to put
 * back.
 *
 * There is no dpd layer to ask what a real power cycle does, because no SoC with
 * an FTM declares a suspend-to-ram state. The clobber stands in for one: the replay
 * is exercised from an arbitrarily corrupted register image rather than from one
 * that merely survived a clock gate.
 *
 * Note which action is expected to bring the peripheral up. This driver does nothing
 * in TURN_ON and configures from RESUME, so that an instance which boots
 * SUSPENDED under runtime PM boots with its clock genuinely gated instead of
 * running behind a PM state that says otherwise. No FTM node is on a power domain
 * either, so TURN_ON never arrives at run time at all and a bring-up placed there
 * would be dead code. The second test asserts that split.
 */

#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include <pm_test/state.h>

#include "ftm.h"

ZTEST(ftm_pm, test_device_suspend_resume)
{
	FTM_FOREACH_CASE(c) {
		struct ftm_image running;
		struct ftm_image after;
		int err;

		if (!ftm_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		/* No runtime PM in this layer, so pm_device_driver_init() resumed it. */
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_true(ftm_gate_is_open(c),
			     "%s: gate closed while PM reports ACTIVE", c->dev->name);

		zassert_ok(ftm_start_and_verify(c), "%s: the counter did not run", c->dev->name);
		ftm_image_read(c, &running);
		ftm_report_hw(c, "before-suspend");

		zassert_ok(pm_device_action_run(c->dev, PM_DEVICE_ACTION_SUSPEND),
			   "%s: SUSPEND action rejected", c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_SUSPENDED);
		ftm_report_hw(c, "after-suspend");

		/*
		 * The claim of this layer: the peripheral is no longer clocked. Nothing is read
		 * out of it here -- an unclocked peripheral does not answer, and reading it to
		 * find out whether it is clocked is the one thing that cannot be done.
		 */
		zassert_false(ftm_gate_is_open(c), "%s: clock gate still open after SUSPEND",
			      c->dev->name);

		zassert_ok(pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME),
			   "%s: RESUME action rejected", c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		ftm_image_read(c, &after);
		ftm_report_hw(c, "after-resume");

		zassert_true(ftm_gate_is_open(c), "%s: gate still closed after RESUME",
			     c->dev->name);

		/*
		 * The waveform the consumer asked for is back without the consumer asking
		 * again, and the counter it was programmed against is running.
		 */
		zassert_true(ftm_image_equal(&after, &running),
			     "%s: RESUME left SC=0x%08x MOD=0x%08x CnV=0x%08x, want "
			     "0x%08x/0x%08x/0x%08x", c->dev->name, after.sc, after.mod,
			     after.cnv, running.sc, running.mod, running.cnv);
		zassert_true(ftm_advances(c), "%s: the counter is not running after RESUME",
			     c->dev->name);

		/* A redundant action must be refused, not obeyed and not fatal. */
		err = pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME);
		zassert_equal(err, -EALREADY, "%s: second RESUME returned %d, want -EALREADY",
			      c->dev->name, err);
	}
}

ZTEST(ftm_pm, test_device_resume_rebuilds_the_register_block)
{
	FTM_FOREACH_CASE(c) {
		struct ftm_image running;
		struct ftm_image clobbered;
		struct ftm_image after_resume;
		bool gate_after_turn_on;
		int suspend_rc;
		int turn_off_rc;
		int turn_on_rc;
		int resume_rc;

		if (!ftm_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_ok(ftm_start_and_verify(c), "%s: the counter did not run", c->dev->name);
		ftm_image_read(c, &running);

		/*
		 * Collected in one go and asserted afterwards, so that a failure cannot hand
		 * the next test a device the core still records as OFF.
		 */
		suspend_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_SUSPEND);

		ftm_clobber(c, &clobbered);

		turn_off_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_TURN_OFF);

		turn_on_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_TURN_ON);
		gate_after_turn_on = ftm_gate_is_open(c);

		resume_rc = pm_device_action_run(c->dev, PM_DEVICE_ACTION_RESUME);
		ftm_image_read(c, &after_resume);
		ftm_report_hw(c, "after-resume");

		zassert_ok(suspend_rc, "%s: SUSPEND rejected (%d)", c->dev->name, suspend_rc);
		zassert_ok(turn_off_rc, "%s: TURN_OFF rejected (%d)", c->dev->name, turn_off_rc);
		zassert_ok(turn_on_rc, "%s: TURN_ON rejected (%d)", c->dev->name, turn_on_rc);
		zassert_ok(resume_rc, "%s: RESUME rejected (%d)", c->dev->name, resume_rc);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		/* Before anything else: a clobber that did not take would make the restore
		 * assertion below answerable without the peripheral ever having been disturbed.
		 */
		zassert_false(ftm_image_equal(&clobbered, &running),
			      "%s: the clobber did not take", c->dev->name);

		/*
		 * TURN_ON returns with the core about to record the device as suspended, so
		 * a peripheral left clocked would be one running behind a PM state that says
		 * it is not. Bringing it back is RESUME's job, and it has to bring back the
		 * run-time image rather than a fresh devicetree configuration.
		 */
		zassert_false(gate_after_turn_on, "%s: TURN_ON opened the clock gate",
			      c->dev->name);
		zassert_true(ftm_gate_is_open(c), "%s: RESUME left the clock gate closed",
			     c->dev->name);
		zassert_true(ftm_image_equal(&after_resume, &running),
			     "%s: RESUME left SC=0x%08x MOD=0x%08x CnV=0x%08x, want "
			     "0x%08x/0x%08x/0x%08x", c->dev->name, after_resume.sc,
			     after_resume.mod, after_resume.cnv, running.sc, running.mod,
			     running.cnv);
		zassert_true(ftm_advances(c), "%s: the counter is not running after RESUME",
			     c->dev->name);
	}
}
