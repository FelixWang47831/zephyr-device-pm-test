/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The system layer: the per-device power state constraint.
 *
 * The FTM node lists the states that stop it counting in
 * zephyr,disabling-power-states, declared by the SoC dtsi for every instance, and
 * the driver holds a policy constraint against exactly those states for as long as
 * a channel is driving an output. The window is not one API call -- a PWM output
 * has no completion event -- so the constraint follows the channel setup and the
 * device state together: a runtime-PM suspend that halts the counter hands the lock
 * back, a resume that restarts it takes it again, and a zero-period
 * pwm_set_cycles() hands the channel back and drops it. Without the release on
 * suspend, one pwm_set_cycles() would block those states for the rest of the run
 * even with no consumer left holding a reference.
 *
 * The helpers below iterate substates rather than asking about substate 0 alone.
 * Both SoCs this case runs on expose their deep-sleep family as three
 * suspend-to-idle substates -- the PSTOPO partial-stop options -- so asking only
 * about substate 0 would miss the locks on the other two.
 *
 * Two tests: that the lock matches what the node declares, appears only once there
 * is a running counter to protect, and goes away again when the channel is handed
 * back with a zero period; and that it follows the counter across a suspend and a
 * resume rather than being taken once and kept.
 */

#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/state.h>
#include <zephyr/ztest.h>

#include <pm_test/lowpower.h>

#include "ftm.h"

/*
 * How many substates to ask about. Three covers the PSTOPO options these SoCs
 * declare; a state that has no such substate simply answers false for the ones it
 * does not have, so over-counting is free and under-counting is not.
 */
#define FTM_PM_SUBSTATE_COUNT 3U

/* How many state/substate pairs are locked right now, by anyone. */
static unsigned int locked_states(void)
{
	unsigned int locked = 0;

	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		for (uint8_t sub = 0U; sub < FTM_PM_SUBSTATE_COUNT; sub++) {
			locked += pm_policy_state_lock_is_active(state, sub) ? 1U : 0U;
		}
	}

	return locked;
}

/*
 * How many state/substate pairs this node declares as disabling. Pairs that are
 * listed but disabled in the devicetree are dropped from the generated table, so
 * this can legitimately be smaller than the list in the SoC dtsi. A zero means the
 * node carries no list at all, which is why the tests below guard on it.
 */
static unsigned int declared_states(const struct device *dev)
{
	unsigned int declared = 0;

	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		for (uint8_t sub = 0U; sub < FTM_PM_SUBSTATE_COUNT; sub++) {
			declared += pm_policy_device_is_disabling_state(dev, state, sub) ? 1U : 0U;
		}
	}

	return declared;
}

/* Locked pairs the node does not declare, or declared pairs left unlocked. */
static unsigned int constraint_mismatch(const struct device *dev)
{
	unsigned int mismatch = 0;

	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		for (uint8_t sub = 0U; sub < FTM_PM_SUBSTATE_COUNT; sub++) {
			bool locked = pm_policy_state_lock_is_active(state, sub);
			bool disabling = pm_policy_device_is_disabling_state(dev, state, sub);

			mismatch += (locked != disabling) ? 1U : 0U;
		}
	}

	return mismatch;
}

ZTEST(ftm_pm, test_system_constraint_covers_the_declared_states)
{
	FTM_FOREACH_CASE(c) {
		unsigned int before;
		unsigned int after_get;
		unsigned int while_running;
		unsigned int declared;
		unsigned int mismatch;
		unsigned int after_release;
		unsigned int after_suspend;
		bool running_after_get;
		int set_rc;
		int release_rc;

		declared = declared_states(c->dev);
		if (declared == 0U) {
			/* No state is declared to stop this instance, so there is no
			 * constraint to observe. Said rather than passed quietly.
			 */
			TC_PRINT("%s: declares no disabling power state, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		pm_test_console_quiesce();
		before = locked_states();

		zassert_ok(pm_device_runtime_get(c->dev), "%s: runtime_get rejected",
			   c->dev->name);

		/*
		 * Resumed. Whether that restored a waveform depends on what an earlier test
		 * left in the driver's shadow state: a PWM has no configured-but-stopped
		 * state once pwm_set_cycles() has been called, so the resume replays it and
		 * the counter runs again. So the claim here is the invariant rather than a
		 * fixed number -- the lock is held exactly while a channel is driving an
		 * output, which a driver that took it unconditionally on resume fails
		 * whenever the peripheral comes back idle.
		 */
		pm_test_console_quiesce();
		running_after_get = ftm_running(c);
		after_get = locked_states();

		set_rc = ftm_start_and_verify(c);
		while_running = locked_states();
		mismatch = constraint_mismatch(c->dev);

		/*
		 * Handed back while the device is still active, which is the one release
		 * path that does not go through a suspend. A zero period is what asks for
		 * it; see mcux_ftm_release_channel() in the driver.
		 */
		release_rc = ftm_release_pwm(c);
		after_release = locked_states();

		/* Released before anything is asserted: a failure must not leave a lock
		 * behind, which would silently block those states for the rest of the run.
		 */
		(void)pm_device_runtime_put(c->dev);

		pm_test_console_quiesce();
		after_suspend = locked_states();

		zassert_equal(before, 0U, "%s: %u state(s) were already locked",
			      c->dev->name, before);
		zassert_ok(set_rc, "%s: the counter did not run (%d)", c->dev->name, set_rc);

		zassert_equal(after_get, running_after_get ? declared : 0U,
			      "%s: %u state(s) locked just after the resume, counter %s",
			      c->dev->name, after_get,
			      running_after_get ? "running" : "idle");
		zassert_equal(while_running, declared,
			      "%s: %u state(s) locked while counting, %u declared",
			      c->dev->name, while_running, declared);
		zassert_equal(mismatch, 0U,
			      "%s: %u state(s) whose lock does not match the node",
			      c->dev->name, mismatch);
		zassert_ok(release_rc, "%s: the channel could not be handed back (%d)",
			   c->dev->name, release_rc);
		zassert_equal(after_release, 0U,
			      "%s: %u state(s) still locked after the channel was handed back",
			      c->dev->name, after_release);
		zassert_equal(after_suspend, 0U,
			      "%s: %u state(s) still locked after the last put",
			      c->dev->name, after_suspend);
	}
}

ZTEST(ftm_pm, test_system_constraint_follows_the_counter)
{
	FTM_FOREACH_CASE(c) {
		unsigned int declared;
		unsigned int while_running;
		unsigned int while_suspended;
		unsigned int after_resume;
		unsigned int at_end;
		int set_rc;
		bool running_after_resume;

		declared = declared_states(c->dev);
		if (declared == 0U) {
			TC_PRINT("%s: declares no disabling power state, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		pm_test_console_quiesce();

		zassert_ok(pm_device_runtime_get(c->dev), "%s: runtime_get rejected",
			   c->dev->name);
		set_rc = ftm_start_and_verify(c);
		while_running = locked_states();

		/* The last reference goes away, so the driver suspends and the counter
		 * halts -- and with it the reason for the lock.
		 */
		(void)pm_device_runtime_put(c->dev);
		pm_test_console_quiesce();
		while_suspended = locked_states();

		/*
		 * A resume restarts the waveform the consumer was running, so it has to
		 * take the protection back: without it the counter comes back running while
		 * the policy is free to enter a state that halts its clock.
		 */
		zassert_ok(pm_device_runtime_get(c->dev), "%s: second runtime_get rejected",
			   c->dev->name);
		running_after_resume = ftm_advances(c);
		after_resume = locked_states();

		(void)pm_device_runtime_put(c->dev);
		pm_test_console_quiesce();
		at_end = locked_states();

		zassert_ok(set_rc, "%s: the counter did not run (%d)", c->dev->name, set_rc);
		zassert_equal(while_running, declared,
			      "%s: %u state(s) locked while counting, %u declared",
			      c->dev->name, while_running, declared);
		zassert_equal(while_suspended, 0U,
			      "%s: %u state(s) still locked while suspended", c->dev->name,
			      while_suspended);
		zassert_true(running_after_resume,
			     "%s: the counter is not running after resume", c->dev->name);
		zassert_equal(after_resume, declared,
			      "%s: %u state(s) locked after the waveform came back, %u declared",
			      c->dev->name, after_resume, declared);
		zassert_equal(at_end, 0U,
			      "%s: %u state(s) leaked past the end of the test", c->dev->name,
			      at_end);
	}
}
