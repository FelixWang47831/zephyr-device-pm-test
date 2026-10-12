/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The sysmanaged and dpd layers: the device sweep across one transition.
 *
 * One file for two layers, because the sequence is identical and only the state
 * being entered differs -- and that difference is what separates the two claims.
 * In sysmanaged the sweep's SUSPEND and RESUME are all that happen, so re-enabling
 * the amplifier and replaying its gain is enough. In dpd the CORE domain is power
 * gated: the register block comes back at its reset value and nothing re-runs
 * driver init, so the domain's TURN_ON has to rebuild the devicetree configuration
 * before RESUME can put EN and the gain back on top of it. dpd is the only layer
 * that can catch a driver with no restore path for either half -- it is what the
 * RESUME gain-replay fix this case exists for is actually proving.
 *
 * The application's part is holding the amplifier on across the transition, which
 * is why the sequence below asks for it directly rather than through a reference:
 * this driver never resumes itself, and these layers run with no runtime PM, so an
 * output that was never turned on is not one the sweep owes anything to.
 */

#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include <pm_test/lowpower.h>
#include <pm_test/state.h>

#include "opamp.h"

ZTEST(opamp_pm, test_sweep_across_transition)
{
	uint32_t config;
	uint32_t config_after;
	bool en_after;

	/* No runtime PM in these layers, so the sweep is the only thing that will
	 * suspend the device.
	 */
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_ACTIVE);
	zassert_true(opamp_en(), "EN clear before the transition");
	config = opamp_config_bits();

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	uint32_t gain;
	uint32_t gain_after;

	zassert_ok(opamp_configure_gain(OPAMP_GAIN_8), "opamp_set_gain() failed before the "
							"transition");
	gain = opamp_gain_fields();
#endif
	opamp_report_hw("before");

	pm_test_enter_transition();

	/* Sampled before the console is revived: reviving it takes time, and the
	 * question is what the block looked like the moment the sweep handed it back.
	 */
	config_after = opamp_config_bits();
	en_after = opamp_en();
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	gain_after = opamp_gain_fields();
#endif

	pm_test_console_resume(PM_TEST_CONSOLE_REINIT_PINMUX);
	TC_PRINT("resumed from %s\n", PM_TEST_TRANSITION_NAME);
	opamp_report_hw("after-resume");

	/*
	 * pm_resume_devices() walks the devices it suspended in init order, so the
	 * core domain's TURN_ON has already run by the time the opamp's own RESUME
	 * does. The two together have to leave both EN and the gain the consumer last
	 * asked for.
	 */
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_ACTIVE);
	zassert_equal(config_after, config,
		      "the configuration did not survive the transition: 0x%08x, want 0x%08x",
		      config_after, config);
	zassert_true(en_after, "EN was not put back for an amplifier that never let go");
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	zassert_equal(gain_after, gain,
		      "the gain did not survive the transition: 0x%08x, want 0x%08x",
		      gain_after, gain);
#endif
}
