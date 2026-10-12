/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The control: this case really does drive the opamp.
 *
 * Compiled into every layer. If this fails, no verdict from the layer under test
 * means anything -- which is the whole reason the baseline layer exists as a layer.
 */

#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include "opamp.h"

ZTEST(opamp_pm, test_control_en)
{
	bool held = false;
	bool en;
	uint32_t config;
#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	int gain_rc;
#endif

	/*
	 * Where runtime PM owns the device it boots suspended, and this driver takes
	 * no reference of its own: being ACTIVE is the only thing that turns the
	 * amplifier on, so the control has to ask for the device explicitly. That an
	 * unwrapped call is refused instead of being served is test_runtime.c's claim,
	 * not a premise of the control.
	 */
	if (pm_device_runtime_is_enabled(OPAMP_DEV)) {
		zassert_ok(pm_device_runtime_get(OPAMP_DEV),
			   "could not resume the device for the control");
		held = true;
	}

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	gain_rc = opamp_configure_gain(OPAMP_GAIN_2);
#endif

	en = opamp_en();
	config = opamp_config_bits();
	opamp_report_hw("control");

	if (held) {
		/* Before the assertions below: a failure must not leak the reference. */
		(void)pm_device_runtime_put(OPAMP_DEV);
	}

	zassert_true(en, "the EN bit is clear while the opamp is ACTIVE");

	/*
	 * What the driver wrote from the devicetree. A cleared register is what a
	 * reset leaves, and every later layer tells "restored" from "came back from
	 * reset" by comparing against this, so a case in which it is empty proves
	 * nothing anywhere.
	 */
	zassert_not_equal(config, 0U, "the register carries no configuration once ACTIVE");

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
	zassert_ok(gain_rc, "opamp_set_gain() failed (%d)", gain_rc);
	zassert_not_equal(opamp_gain_fields(), 0U,
			  "the gain fields are still 0 after requesting a non-default gain");
#endif
}
