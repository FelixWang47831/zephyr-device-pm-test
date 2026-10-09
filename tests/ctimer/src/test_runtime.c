/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The runtime layer: the consumer owns the counter, and says so with a
 *        reference.
 *
 * This driver takes no runtime PM reference of its own. A counter runs for as long
 * as the application wants a time base, and there is no completion event a put()
 * could hang on, so wrapping each API call in its own get/put would stop the
 * counter the moment the call returned. The reference is the consumer's.
 *
 * What this layer is really here to catch is the boot state. pm_device_driver_init()
 * runs TURN_ON and then stops short of RESUME when runtime PM is about to own the
 * device, so a driver that leaves the block clocked in TURN_ON boots it running while
 * the core records it as SUSPENDED -- green everywhere else, and wrong. This driver's
 * TURN_ON configures the block and gates it again before returning, and the first
 * test below is the check.
 *
 * Two tests. The first has to run first: it reads the state the device booted in,
 * and the second leaves it suspended.
 */

#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include <pm_test/state.h>

#include "ctimer.h"

ZTEST(ctimer_pm, test_runtime_boots_without_a_consumer)
{
	CTIMER_FOREACH_CASE(c) {
		enum pm_device_state state;

		zassert_true(pm_device_runtime_is_enabled(c->dev),
			     "%s: runtime PM is not enabled, so this layer tests nothing",
			     c->dev->name);

		/*
		 * Two boot states are legal. Without a power domain the device lands in
		 * SUSPENDED; with one it lands in OFF, because the domain is
		 * runtime-suspended before the CTIMER initialises and TURN_ON was skipped
		 * -- pd-soc-state-change-no-turn-on-under-runtime-pm in docs/findings.md.
		 */
		zassert_ok(pm_device_state_get(c->dev, &state),
			   "%s: pm_device_state_get() failed", c->dev->name);
		zassert_true((state == PM_DEVICE_STATE_SUSPENDED) ||
				     (state == PM_DEVICE_STATE_OFF),
			     "%s booted %s, want SUSPENDED or OFF under runtime PM",
			     c->dev->name, pm_device_state_str(state));

		if (!ctimer_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, nothing asserted\n",
				 c->dev->name);
			continue;
		}

		/* The claim. Nothing may be clocked before a consumer has asked for it. */
		zassert_false(ctimer_gate_is_open(c),
			      "%s: clock gate open before any consumer took a reference",
			      c->dev->name);
	}
}

ZTEST(ctimer_pm, test_runtime_reference_counting)
{
	CTIMER_FOREACH_CASE(c) {
		struct ctimer_image armed;
		struct ctimer_image after;

		zassert_ok(pm_device_runtime_get(c->dev), "%s: runtime_get rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		if (ctimer_gate_observable(c)) {
			zassert_true(ctimer_gate_is_open(c),
				     "%s: gate closed while PM reports ACTIVE", c->dev->name);
		}

		zassert_ok(ctimer_start_and_verify(c), "%s: counter did not start under a held "
			   "reference", c->dev->name);
		zassert_ok(ctimer_arm_alarm(c, 50U), "%s: could not arm the alarm", c->dev->name);
		ctimer_image_read(c, &armed);

		/* A second reference must not be a second resume, and releasing it must not
		 * stop a counter somebody else is still holding.
		 */
		zassert_ok(pm_device_runtime_get(c->dev), "%s: nested runtime_get rejected",
			   c->dev->name);
		zassert_ok(pm_device_runtime_put(c->dev), "%s: first runtime_put rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);
		zassert_true(ctimer_advances(c), "%s: counter stopped while a reference was held",
			     c->dev->name);

		zassert_ok(pm_device_runtime_put(c->dev), "%s: last runtime_put rejected",
			   c->dev->name);
		zassert_pm_state(c->dev, PM_DEVICE_STATE_SUSPENDED);
		ctimer_report_hw(c, "after-put");

		if (ctimer_gate_observable(c)) {
			zassert_false(ctimer_gate_is_open(c),
				      "%s: clock gate still open after the last runtime_put",
				      c->dev->name);
		}

		/*
		 * And the consumer gets its counter back by taking the reference again,
		 * without re-arming: what it programmed is the driver's to remember across
		 * a suspend the consumer did not ask for.
		 */
		zassert_ok(pm_device_runtime_get(c->dev), "%s: runtime_get after the put rejected",
			   c->dev->name);
		ctimer_image_read(c, &after);
		ctimer_report_hw(c, "after-reget");

		zassert_true(ctimer_image_equal(&after, &armed),
			     "%s: the armed alarm did not come back with the reference: "
			     "TCR=0x%08x MCR=0x%08x MR=0x%08x, want 0x%08x/0x%08x/0x%08x",
			     c->dev->name, after.tcr, after.mcr, after.mr, armed.tcr, armed.mcr,
			     armed.mr);
		zassert_true(ctimer_alarm_wait(c, 500U), "%s: the alarm did not fire after the "
			     "counter came back", c->dev->name);

		/* Left as it was found, so this test's counter does not outlive it. */
		(void)ctimer_cancel_alarm(c);
		(void)ctimer_stop(c);
		zassert_ok(pm_device_runtime_put(c->dev), "%s: final runtime_put rejected",
			   c->dev->name);
	}
}
