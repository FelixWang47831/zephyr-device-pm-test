/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The system layer: the per-device power state constraint.
 *
 * &opamp0 lists the states that cut the amplifier's bias in
 * zephyr,disabling-power-states (see constraints.overlay). Like the LPCMP driver
 * and unlike LPDAC's, this driver takes no policy lock of its own -- an opamp is
 * useful for as long as the application wants its output, not for the length of
 * one call -- so holding the constraint is the application's job, and both tests
 * here are written the way an application has to write it.
 */

#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/state.h>
#include <zephyr/ztest.h>

#include <pm_test/lowpower.h>

#include "opamp.h"

ZTEST(opamp_pm, test_system_constraint_covers_the_declared_states)
{
	unsigned int declared = 0;

	pm_test_console_quiesce();

	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		zassert_false(pm_policy_state_lock_is_active(state, 0),
			      "power state %d was already locked before the test", state);
	}

	pm_policy_device_power_lock_get(OPAMP_DEV);

	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		bool disabling = pm_policy_device_is_disabling_state(OPAMP_DEV, state, 0);

		declared += disabling ? 1U : 0U;
		zassert_equal(pm_policy_state_lock_is_active(state, 0), disabling,
			      "power state %d: locked=%d but declared disabling=%d", state,
			      (int)pm_policy_state_lock_is_active(state, 0), (int)disabling);
	}

	/*
	 * Without this the loop above would compare "nothing locked" against "nothing
	 * declared" and pass while proving nothing -- which is exactly what a
	 * constraints.overlay that failed to reach &opamp0 looks like. States that are
	 * listed but disabled in the devicetree are dropped from the generated table,
	 * so on a board where all of them are disabled this is the check that fires.
	 */
	zassert_true(declared > 0,
		     "no disabling power state reached the constraint table; is "
		     "constraints.overlay applied to this board's opamp node?");

	pm_policy_device_power_lock_put(OPAMP_DEV);

	pm_test_console_quiesce();
	for (enum pm_state state = PM_STATE_ACTIVE; state < PM_STATE_COUNT; state++) {
		zassert_false(pm_policy_state_lock_is_active(state, 0),
			      "power state %d is still locked after the constraint was "
			      "released", state);
	}
}

ZTEST(opamp_pm, test_system_opamp_usable_under_the_constraint)
{
	bool en;
	uint32_t config;
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	int gain_rc;
	uint32_t gain;
#endif

	/* The pair an application needs: the reference keeps the block powered, the
	 * constraint keeps the policy from picking a state that would unpower it.
	 */
	pm_policy_device_power_lock_get(OPAMP_DEV);
	zassert_ok(pm_device_runtime_get(OPAMP_DEV), "runtime_get rejected");

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	gain_rc = opamp_configure_gain(OPAMP_GAIN_2);
	gain = opamp_gain_fields();
#endif
	en = opamp_en();
	config = opamp_config_bits();

	/* Released before anything is asserted: a failure must not leave a lock or a
	 * reference behind, either of which would silently distort the rest of the run.
	 */
	(void)pm_device_runtime_put(OPAMP_DEV);
	pm_policy_device_power_lock_put(OPAMP_DEV);

	zassert_true(en, "EN clear while a runtime reference was held");
	zassert_not_equal(config, 0U, "the register carries no configuration while ACTIVE");
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	zassert_ok(gain_rc, "opamp_set_gain() failed (%d)", gain_rc);
	zassert_not_equal(gain, 0U,
			  "the gain fields are still 0 after requesting a non-default gain");
#endif
}
