/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The system layer: the per-device power state constraint.
 *
 * The CTIMER node lists the states that stop it counting in
 * zephyr,disabling-power-states (see constraints.overlay), and the driver holds a
 * policy constraint against exactly those states for as long as the counter is
 * actually counting. The window is not one API call -- a counter has no completion
 * event -- so the constraint follows the counter and the device state together: a
 * runtime-PM suspend that stops the counter hands the lock back, and a resume that
 * restarts it takes it again. Without that, one counter_start() would block those
 * states for the rest of the run even with no consumer left holding a reference.
 *
 * Two tests: that the lock matches what the node declares and appears only once
 * there is a running counter to protect, and that it follows the counter across a
 * suspend and a resume rather than being taken once and kept.
 */

#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/state.h>
#include <zephyr/ztest.h>

#include <pm_test/lowpower.h>

#include "ctimer.h"

/* How many power states are locked right now, by anyone. */
static unsigned int locked_states(void)
{
	unsigned int locked = 0;

	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		locked += pm_policy_state_lock_is_active(state, 0) ? 1U : 0U;
	}

	return locked;
}

/*
 * How many power states this node declares as disabling. States that are listed but
 * disabled in the devicetree are dropped from the generated table, so this can
 * legitimately be smaller than the list in constraints.overlay. A zero means the
 * overlay never reached the node, which is why the tests below guard on it.
 */
static unsigned int declared_states(const struct device *dev)
{
	unsigned int declared = 0;

	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		declared += pm_policy_device_is_disabling_state(dev, state, 0) ? 1U : 0U;
	}

	return declared;
}

/* Locked states that the node does not declare, or declared states left unlocked. */
static unsigned int constraint_mismatch(const struct device *dev)
{
	unsigned int mismatch = 0;

	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		bool locked = pm_policy_state_lock_is_active(state, 0);
		bool disabling = pm_policy_device_is_disabling_state(dev, state, 0);

		mismatch += (locked != disabling) ? 1U : 0U;
	}

	return mismatch;
}

ZTEST(ctimer_pm, test_system_constraint_covers_the_declared_states)
{
	CTIMER_FOREACH_CASE(c) {
		unsigned int before;
		unsigned int after_get;
		unsigned int while_counting;
		unsigned int declared;
		unsigned int mismatch;
		unsigned int after_stop;
		int start_rc;

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

		/* Resumed, but the counter is not counting yet: nothing to protect. */
		pm_test_console_quiesce();
		after_get = locked_states();

		start_rc = ctimer_start_and_verify(c);
		while_counting = locked_states();
		mismatch = constraint_mismatch(c->dev);

		/* Released before anything is asserted: a failure must not leave a lock
		 * behind, which would silently block those states for the rest of the run.
		 */
		(void)ctimer_stop(c);
		(void)pm_device_runtime_put(c->dev);

		pm_test_console_quiesce();
		after_stop = locked_states();

		zassert_equal(before, 0U, "%s: %u power state(s) were already locked",
			      c->dev->name, before);
		zassert_ok(start_rc, "%s: counter did not start and count (%d)", c->dev->name,
			   start_rc);

		zassert_equal(after_get, 0U,
			      "%s: %u power state(s) locked by a resumed but stopped counter",
			      c->dev->name, after_get);
		zassert_equal(while_counting, declared,
			      "%s: %u state(s) locked while counting, %u declared", c->dev->name,
			      while_counting, declared);
		zassert_equal(mismatch, 0U,
			      "%s: %u power state(s) whose lock does not match the node",
			      c->dev->name, mismatch);
		zassert_equal(after_stop, 0U, "%s: %u power state(s) still locked after stop",
			      c->dev->name, after_stop);
	}
}

ZTEST(ctimer_pm, test_system_constraint_follows_the_counter)
{
	CTIMER_FOREACH_CASE(c) {
		unsigned int declared;
		unsigned int while_counting;
		unsigned int while_suspended;
		unsigned int after_resume;
		unsigned int at_end;
		int start_rc;
		bool counting_after_resume;

		declared = declared_states(c->dev);
		if (declared == 0U) {
			TC_PRINT("%s: declares no disabling power state, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		pm_test_console_quiesce();

		zassert_ok(pm_device_runtime_get(c->dev), "%s: runtime_get rejected",
			   c->dev->name);
		start_rc = ctimer_start_and_verify(c);
		while_counting = locked_states();

		/* The last reference goes away, so the driver suspends and the counter
		 * stops -- and with it the reason for the lock.
		 */
		(void)pm_device_runtime_put(c->dev);
		pm_test_console_quiesce();
		while_suspended = locked_states();

		/*
		 * A resume restarts the counter the consumer was running, so it has to
		 * take the protection back: without it the counter comes back counting
		 * while the policy is free to enter a state that stops its clock.
		 */
		zassert_ok(pm_device_runtime_get(c->dev), "%s: second runtime_get rejected",
			   c->dev->name);
		counting_after_resume = ctimer_advances(c);
		after_resume = locked_states();

		(void)ctimer_stop(c);
		(void)pm_device_runtime_put(c->dev);
		pm_test_console_quiesce();
		at_end = locked_states();

		zassert_ok(start_rc, "%s: counter did not start and count (%d)", c->dev->name,
			   start_rc);
		zassert_equal(while_counting, declared,
			      "%s: %u state(s) locked while counting, %u declared", c->dev->name,
			      while_counting, declared);
		zassert_equal(while_suspended, 0U,
			      "%s: %u power state(s) still locked while suspended", c->dev->name,
			      while_suspended);
		zassert_true(counting_after_resume, "%s: the counter is not running after resume",
			     c->dev->name);
		zassert_equal(after_resume, declared,
			      "%s: %u state(s) locked after the counter came back, %u declared",
			      c->dev->name, after_resume, declared);
		zassert_equal(at_end, 0U, "%s: %u power state(s) leaked past the end of the test",
			      c->dev->name, at_end);
	}
}
