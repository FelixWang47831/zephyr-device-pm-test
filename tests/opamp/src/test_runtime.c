/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The runtime layer: the consumer owns the amplifier, and says so with a
 *        reference.
 *
 * This driver takes no runtime PM reference of its own: being ACTIVE is the only
 * thing that turns the amplifier on, and there is no completion event a put()
 * could hang on. The reference is the consumer's, and a call that arrives without
 * one is refused.
 *
 * Two tests. The first has to run first: it reads the state the device booted in,
 * and the second leaves it suspended.
 */

#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include <pm_test/state.h>

#include "opamp.h"

ZTEST(opamp_pm, test_runtime_boots_without_a_consumer)
{
	enum pm_device_state state;

	zassert_true(pm_device_runtime_is_enabled(OPAMP_DEV),
		     "runtime PM is not enabled on %s, so this layer tests nothing",
		     OPAMP_DEV->name);

	/*
	 * Two boot states are legal. Without a power domain the device lands in
	 * SUSPENDED: pm_device_driver_init() ran TURN_ON and stopped short of RESUME
	 * because runtime PM is about to take over. With a domain it lands in OFF,
	 * because the domain is runtime-suspended before the opamp initialises and
	 * TURN_ON was skipped -- pd-soc-state-change-no-turn-on-under-runtime-pm in
	 * docs/findings.md.
	 */
	zassert_ok(pm_device_state_get(OPAMP_DEV, &state), "pm_device_state_get() failed");
	zassert_true((state == PM_DEVICE_STATE_SUSPENDED) || (state == PM_DEVICE_STATE_OFF),
		     "%s booted %s, want SUSPENDED or OFF under runtime PM", OPAMP_DEV->name,
		     pm_device_state_str(state));

	/* Nothing may be driving an output before a consumer has asked for one. */
	zassert_false(opamp_en(), "EN set before any consumer took a reference");

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	int rc = opamp_configure_gain(OPAMP_GAIN_2);

	zassert_equal(rc, -EBUSY, "set_gain without a reference returned %d, want -EBUSY", rc);
#endif
	zassert_false(opamp_en(), "a refused call enabled the amplifier");
}

ZTEST(opamp_pm, test_runtime_reference_counting)
{
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	uint32_t gain;
#endif

	zassert_ok(pm_device_runtime_get(OPAMP_DEV), "runtime_get rejected");
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_ACTIVE);
	zassert_true(opamp_en(), "EN clear while PM reports ACTIVE");

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	zassert_ok(opamp_configure_gain(OPAMP_GAIN_2), "set_gain under a held reference failed");
	gain = opamp_gain_fields();
#endif

	/* A second reference must not be a second resume, and releasing it must not
	 * disable an amplifier somebody else is still holding.
	 */
	zassert_ok(pm_device_runtime_get(OPAMP_DEV), "nested runtime_get rejected");
	zassert_ok(pm_device_runtime_put(OPAMP_DEV), "first runtime_put rejected");
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_ACTIVE);
	zassert_true(opamp_en(), "the amplifier was disabled while a reference was held");

	zassert_ok(pm_device_runtime_put(OPAMP_DEV), "last runtime_put rejected");
	zassert_pm_state(OPAMP_DEV, PM_DEVICE_STATE_SUSPENDED);
	opamp_report_hw("after-put");
	zassert_false(opamp_en(), "EN still set after the last runtime_put");

	/*
	 * And the consumer gets its gain back by taking the reference again, without
	 * calling set_gain() a second time: what it asked for is the driver's to
	 * remember across a suspend it did not ask for.
	 */
	zassert_ok(pm_device_runtime_get(OPAMP_DEV), "runtime_get after the put rejected");
	opamp_report_hw("after-reget");
	zassert_true(opamp_en(), "the amplifier did not come back with the reference");
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	zassert_equal(opamp_gain_fields(), gain,
		      "gain changed across the suspend: 0x%08x, want 0x%08x",
		      opamp_gain_fields(), gain);
#endif

	/* Left as it was found, so this test's output does not outlive it. */
	zassert_ok(pm_device_runtime_put(OPAMP_DEV), "final runtime_put rejected");
}
