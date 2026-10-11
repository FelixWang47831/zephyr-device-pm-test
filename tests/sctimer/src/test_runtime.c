/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The runtime layer: the consumer owns the output, and says so with a
 *        reference.
 *
 * This driver takes no runtime PM reference of its own. A PWM output drives until the
 * consumer changes it, and there is no completion event a put() could hang on, so
 * wrapping each API call in its own get/put would stop the output the moment the call
 * returned. The reference is the consumer's -- and because it is, a call that arrives
 * while nobody holds one cannot reach the peripheral and is refused rather than
 * silently writing a register behind a closed clock gate.
 *
 * What this layer also catches is the boot state. pm_device_driver_init() runs TURN_ON
 * and then stops short of RESUME when runtime PM is about to own the device, so a
 * driver that leaves the block clocked in TURN_ON boots it running while the core
 * records it as SUSPENDED. This driver's TURN_ON configures the block, parks it and
 * gates it again before returning, and the first test below is the check.
 *
 * Three tests. The first has to run first: it reads the state the device booted in,
 * and the others leave it suspended.
 */

#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include <pm_test/state.h>

#include "sctimer.h"

ZTEST(sctimer_pm, test_runtime_boots_without_a_consumer)
{
	SCTIMER_FOREACH_CASE(c) {
		enum pm_device_state state;

		zassert_true(pm_device_runtime_is_enabled(c->dev),
			     "%s: runtime PM is not enabled on this device", c->dev->name);

		/*
		 * Two boot states are legal. Without a power domain the device lands in
		 * SUSPENDED; with one it lands in OFF, because the domain is
		 * runtime-suspended before the SCTimer initialises and TURN_ON was
		 * skipped -- pd-soc-state-change-no-turn-on-under-runtime-pm in
		 * docs/findings.md.
		 */
		zassert_ok(pm_device_state_get(c->dev, &state),
			   "%s: pm_device_state_get() failed", c->dev->name);
		zassert_true((state == PM_DEVICE_STATE_SUSPENDED) ||
				     (state == PM_DEVICE_STATE_OFF),
			     "%s booted %s, want SUSPENDED or OFF under runtime PM",
			     c->dev->name, pm_device_state_str(state));

		if (!sctimer_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		/* The claim. Nothing may be clocked before a consumer has asked for it. */
		zassert_false(sctimer_gate_is_open(c),
			      "%s: clock gate open before any consumer took a reference",
			      c->dev->name);
	}
}

ZTEST(sctimer_pm, test_runtime_api_refuses_a_suspended_device)
{
	SCTIMER_FOREACH_CASE(c) {
		struct sctimer_peek before;
		struct sctimer_peek after;
		int err;

		zassert_pm_state(c->dev, PM_DEVICE_STATE_SUSPENDED);

		/*
		 * What the registers hold before the refused call, so the refusal can be
		 * shown to have been a refusal and not a write that happened to fail.
		 */
		sctimer_peek_gated(c, &before);

		err = sctimer_configure(c);

		sctimer_peek_gated(c, &after);

		zassert_equal(err, -EBUSY,
			      "%s: pwm_set_cycles() on a suspended device returned %d, want "
			      "-EBUSY", c->dev->name, err);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_SUSPENDED);

		if (sctimer_gate_observable(c)) {
			zassert_false(sctimer_gate_is_open(c),
				      "%s: the refused call left the clock gate open",
				      c->dev->name);
		}

		zassert_true(sctimer_image_equal(&after.image, &before.image),
			     "%s: the refused call changed the registers", c->dev->name);
		zassert_false(after.running, "%s: the refused call started the counter",
			      c->dev->name);
	}
}

ZTEST(sctimer_pm, test_runtime_reference_counting)
{
	SCTIMER_FOREACH_CASE(c) {
		struct sctimer_image configured;
		struct sctimer_image after;

		zassert_ok(pm_device_runtime_get(c->dev), "%s: runtime_get rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		if (sctimer_gate_observable(c)) {
			zassert_true(sctimer_gate_is_open(c),
				     "%s: gate closed while PM reports ACTIVE", c->dev->name);
		}

		zassert_ok(sctimer_configure(c), "%s: pwm_set_cycles() failed under a held "
			   "reference", c->dev->name);
		sctimer_image_read(c, &configured);
		zassert_true(sctimer_output_toggles(c), "%s: the output is not toggling under a "
			     "held reference", c->dev->name);

		/* A second reference must not be a second resume, and releasing it must not
		 * stop an output somebody else is still holding.
		 */
		zassert_ok(pm_device_runtime_get(c->dev), "%s: nested runtime_get rejected",
			   c->dev->name);
		zassert_ok(pm_device_runtime_put(c->dev), "%s: first runtime_put rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_true(sctimer_advances(c), "%s: counter stopped while a reference was held",
			     c->dev->name);

		zassert_ok(pm_device_runtime_put(c->dev), "%s: last runtime_put rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_SUSPENDED);
		sctimer_report_hw(c, "after-put");

		if (sctimer_gate_observable(c)) {
			zassert_false(sctimer_gate_is_open(c),
				      "%s: clock gate still open after the last runtime_put",
				      c->dev->name);
		}

		/*
		 * And the consumer gets its output back by taking the reference again,
		 * without reconfiguring: what it programmed is the driver's to remember
		 * across a suspend the consumer did not ask for.
		 */
		zassert_ok(pm_device_runtime_get(c->dev),
			   "%s: runtime_get after the put rejected", c->dev->name);
		sctimer_image_read(c, &after);
		sctimer_report_hw(c, "after-reget");

		zassert_true(sctimer_image_equal(&after, &configured),
			     "%s: the channel did not come back with the reference: "
			     "MATCHREL0=0x%08x MATCHREL1=0x%08x SET=0x%08x CLR=0x%08x, want "
			     "0x%08x/0x%08x/0x%08x/0x%08x", c->dev->name, after.matchrel_period,
			     after.matchrel_pulse, after.out_set, after.out_clr,
			     configured.matchrel_period, configured.matchrel_pulse,
			     configured.out_set, configured.out_clr);
		zassert_true(sctimer_output_toggles(c),
			     "%s: the output is not toggling after the counter came back",
			     c->dev->name);

		/* Left as it was found, so this test's output does not outlive it. */
		zassert_ok(pm_device_runtime_put(c->dev), "%s: final runtime_put rejected",
			   c->dev->name);
	}
}
