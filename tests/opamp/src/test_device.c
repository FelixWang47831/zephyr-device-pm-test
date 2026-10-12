/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The device layer: the driver's PM callbacks, driven directly.
 *
 * Two tests, and they are two because they are about two different halves of the
 * callback. The first is the SUSPEND/RESUME round trip, which owns the EN bit and,
 * on the gain-capable variant, the gain fields RESUME replays. The second is
 * TURN_OFF/TURN_ON, which owns the devicetree configuration half of the register
 * block -- and since nothing in this layer really cut power to it, the block is
 * cleared by hand so that TURN_ON has something to restore. What a real power
 * cycle does is asked in src/test_lowpower.c under the dpd layer.
 */

#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include <pm_test/state.h>

#include "opamp.h"

ZTEST(opamp_pm, test_device_suspend_resume)
{
	uint32_t config;

	/* No runtime PM in this layer, so pm_device_driver_init() resumed it. */
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_ACTIVE);
	zassert_true(opamp_en(), "EN clear while PM reports ACTIVE");
	config = opamp_config_bits();

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	uint32_t gain;
	int set_gain_rc;

	zassert_ok(opamp_configure_gain(OPAMP_GAIN_4), "opamp_set_gain() failed before SUSPEND");
	gain = opamp_gain_fields();
#endif

	zassert_ok(pm_device_action_run(OPAMP_DEV, PM_DEVICE_ACTION_SUSPEND),
		   "SUSPEND action rejected");
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_SUSPENDED);
	zassert_false(opamp_en(), "EN still set after SUSPEND");
	zassert_equal(opamp_config_bits(), config,
		      "SUSPEND changed the configuration: 0x%08x -> 0x%08x", config,
		      opamp_config_bits());

	/*
	 * The API contract while the device is not active. Returning an error is the
	 * whole point: the alternative is writing the gain fields behind PM's back,
	 * which either is overwritten the moment the amplifier is enabled again or is
	 * thrown away by the next power cycle, and the caller is told neither.
	 */
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	set_gain_rc = opamp_configure_gain(OPAMP_GAIN_8);
	zassert_equal(set_gain_rc, -EBUSY,
		      "set_gain against a SUSPENDED device returned %d, want -EBUSY",
		      set_gain_rc);
	zassert_equal(opamp_gain_fields(), gain,
		      "a refused set_gain changed the stored gain fields");
#endif
	zassert_false(opamp_en(), "a refused call re-enabled the amplifier");

	zassert_ok(pm_device_action_run(OPAMP_DEV, PM_DEVICE_ACTION_RESUME),
		   "RESUME action rejected");
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_ACTIVE);
	opamp_report_hw("after-resume");

	/* The configuration comes back without the consumer doing anything, and so
	 * does the gain the consumer last asked for -- this is the fix under test.
	 */
	zassert_true(opamp_en(), "RESUME did not re-enable the amplifier");
	zassert_equal(opamp_config_bits(), config,
		      "RESUME left a different configuration: 0x%08x, want 0x%08x",
		      opamp_config_bits(), config);
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	zassert_equal(opamp_gain_fields(), gain,
		      "RESUME left gain fields 0x%08x, want the last requested gain 0x%08x",
		      opamp_gain_fields(), gain);
#endif

	/* A redundant action must be refused, not obeyed and not fatal. */
	int err = pm_device_action_run(OPAMP_DEV, PM_DEVICE_ACTION_RESUME);

	zassert_equal(err, -EALREADY, "second RESUME returned %d, want -EALREADY", err);
}

ZTEST(opamp_pm, test_device_turn_on_rebuilds_the_register_block)
{
	uint32_t config;
	uint32_t config_clobbered;
	uint32_t config_after_turn_on;
	bool en_after_turn_on;
	bool en_after_resume;
	int suspend_rc;
	int turn_off_rc;
	int turn_on_rc;
	int resume_rc;

	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_ACTIVE);
	config = opamp_config_bits();

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	uint32_t gain;
	uint32_t gain_after_turn_on;
	uint32_t gain_after_resume;

	zassert_ok(opamp_configure_gain(OPAMP_GAIN_16), "opamp_set_gain() failed");
	gain = opamp_gain_fields();
#endif

	/* Collected in one go and asserted afterwards, so that a failure cannot hand
	 * the next test a device the core still records as OFF.
	 */
	suspend_rc = pm_device_action_run(OPAMP_DEV, PM_DEVICE_ACTION_SUSPEND);
	opamp_clobber();
	config_clobbered = opamp_config_bits();

	turn_off_rc = pm_device_action_run(OPAMP_DEV, PM_DEVICE_ACTION_TURN_OFF);

	turn_on_rc = pm_device_action_run(OPAMP_DEV, PM_DEVICE_ACTION_TURN_ON);
	config_after_turn_on = opamp_config_bits();
	en_after_turn_on = opamp_en();
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	gain_after_turn_on = opamp_gain_fields();
#endif
	opamp_report_hw("after-turn-on");

	resume_rc = pm_device_action_run(OPAMP_DEV, PM_DEVICE_ACTION_RESUME);
	en_after_resume = opamp_en();
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	gain_after_resume = opamp_gain_fields();
#endif
	opamp_report_hw("after-resume");

	zassert_ok(suspend_rc, "SUSPEND rejected (%d)", suspend_rc);
	zassert_ok(turn_off_rc, "TURN_OFF rejected (%d)", turn_off_rc);
	zassert_ok(turn_on_rc, "TURN_ON rejected (%d)", turn_on_rc);
	zassert_ok(resume_rc, "RESUME rejected (%d)", resume_rc);
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_ACTIVE);

	/* Before anything else: a clobber that did not take would make the restore
	 * assertions below pass without the driver having rebuilt a thing.
	 */
	zassert_not_equal(config_clobbered, config,
			  "the clobber did not take, so this test proves nothing");

	zassert_equal(config_after_turn_on, config,
		      "TURN_ON left 0x%08x, want the devicetree configuration 0x%08x",
		      config_after_turn_on, config);

	/*
	 * And it has to stop there. TURN_ON returns with the core about to record the
	 * device as suspended, so a block left enabled would be driving an output
	 * that PM believes is quiet; enabling it is RESUME's job.
	 */
	zassert_false(en_after_turn_on, "TURN_ON left the amplifier enabled");
	zassert_true(en_after_resume, "RESUME did not enable the amplifier");

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	/*
	 * The gain fields are runtime state, not devicetree configuration -- TURN_ON
	 * does not own them, and after a clobber they read back as 0 until something
	 * replays the last gain this driver applied.
	 */
	zassert_equal(gain_after_turn_on, 0U,
		      "TURN_ON restored gain fields 0x%08x; it owns devicetree "
		      "configuration, not driver state", gain_after_turn_on);
	zassert_equal(gain_after_resume, gain,
		      "RESUME left gain fields 0x%08x, want the last requested gain 0x%08x",
		      gain_after_resume, gain);
#endif
}
