/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The runtime layer: the consumer owns the waveform, and says so with a
 *        reference.
 *
 * This driver takes no runtime PM reference of its own. A PWM output runs for as
 * long as the application wants a waveform, and there is no completion event a
 * put() could hang on, so wrapping each API call in its own get/put would stop
 * the output the moment the call returned. The reference is the consumer's.
 *
 * What this layer catches is the boot state. pm_device_driver_init() runs TURN_ON
 * and then stops short of RESUME when runtime PM is about to own the device, so a
 * driver that brings the peripheral up in TURN_ON boots it clocked while the core
 * records it as SUSPENDED. This driver brings the peripheral up in RESUME for that
 * reason, and the first test below is the check.
 *
 * Two tests. The first has to run first: it reads the state the device booted in,
 * and the second leaves it suspended.
 */

#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include <pm_test/state.h>

#include "ftm.h"

ZTEST(ftm_pm, test_runtime_boots_without_a_consumer)
{
	FTM_FOREACH_CASE(c) {
		enum pm_device_state state;

		zassert_true(pm_device_runtime_is_enabled(c->dev),
			     "%s: runtime PM is not enabled on this device", c->dev->name);

		/*
		 * Two boot states are legal. Without a power domain the device lands in
		 * SUSPENDED; with one it would land in OFF, because the domain is
		 * runtime-suspended before the FTM initialises and TURN_ON is skipped. No
		 * FTM node is on a domain today, so SUSPENDED is what these boards show.
		 */
		zassert_ok(pm_device_state_get(c->dev, &state),
			   "%s: pm_device_state_get() failed", c->dev->name);
		zassert_true((state == PM_DEVICE_STATE_SUSPENDED) ||
				     (state == PM_DEVICE_STATE_OFF),
			     "%s booted %s, want SUSPENDED or OFF under runtime PM",
			     c->dev->name, pm_device_state_str(state));

		if (!ftm_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		/* The claim. Nothing may be clocked before a consumer has asked for it. */
		zassert_false(ftm_gate_is_open(c),
			      "%s: clock gate open before any consumer took a reference",
			      c->dev->name);
	}
}

ZTEST(ftm_pm, test_runtime_reference_counting)
{
	FTM_FOREACH_CASE(c) {
		struct ftm_image running;
		struct ftm_image after;

		zassert_ok(pm_device_runtime_get(c->dev), "%s: runtime_get rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		if (ftm_gate_observable(c)) {
			zassert_true(ftm_gate_is_open(c),
				     "%s: gate closed while PM reports ACTIVE", c->dev->name);
		}

		zassert_ok(ftm_start_and_verify(c),
			   "%s: the counter did not run under a held reference", c->dev->name);
		ftm_image_read(c, &running);

		/* A second reference must not be a second resume, and releasing it must not
		 * stop a waveform somebody else is still holding.
		 */
		zassert_ok(pm_device_runtime_get(c->dev), "%s: nested runtime_get rejected",
			   c->dev->name);
		zassert_ok(pm_device_runtime_put(c->dev), "%s: first runtime_put rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_true(ftm_advances(c),
			     "%s: the counter stopped while a reference was held", c->dev->name);

		zassert_ok(pm_device_runtime_put(c->dev), "%s: last runtime_put rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_SUSPENDED);
		ftm_report_hw(c, "after-put");

		if (ftm_gate_observable(c)) {
			zassert_false(ftm_gate_is_open(c),
				      "%s: clock gate still open after the last runtime_put",
				      c->dev->name);
		}

		/*
		 * And the consumer gets its waveform back by taking the reference again,
		 * without asking for it a second time: what it programmed is the driver's
		 * own to remember across a suspend the consumer did not ask for.
		 */
		zassert_ok(pm_device_runtime_get(c->dev),
			   "%s: runtime_get after the put rejected", c->dev->name);
		ftm_image_read(c, &after);
		ftm_report_hw(c, "after-reget");

		zassert_true(ftm_image_equal(&after, &running),
			     "%s: the waveform did not come back with the reference: "
			     "SC=0x%08x MOD=0x%08x CnV=0x%08x, want 0x%08x/0x%08x/0x%08x",
			     c->dev->name, after.sc, after.mod, after.cnv, running.sc,
			     running.mod, running.cnv);
		zassert_true(ftm_advances(c),
			     "%s: the counter is not running after the reference came back",
			     c->dev->name);

		/* Left as it was found, so this test waveform does not outlive it. */
		zassert_ok(pm_device_runtime_put(c->dev), "%s: final runtime_put rejected",
			   c->dev->name);
	}
}
