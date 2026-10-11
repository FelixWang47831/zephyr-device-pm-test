/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The sysmanaged and dpd layers: the device sweep across one transition.
 *
 * One file for two layers, because the sequence is identical and only the state being
 * entered differs. In sysmanaged the sweep's SUSPEND and RESUME are all that happen,
 * so the block keeps its power and re-opening the clock gate is enough to put the
 * output back. In dpd the CORE domain is power gated: the whole register block comes
 * back at its reset value, the SYSCON clock gate with it, and nothing re-runs driver
 * init -- so TURN_ON has to rebuild the block from the devicetree and replay the
 * channel the consumer configured.
 *
 * Which means the register image cannot tell the two apart: this driver replays, so a
 * restored image is what both layers owe. The discriminator is a witness value
 * written into a match reload register the driver never touches (see
 * sctimer_witness_write()): it survives a suspend and is gone after a power cycle.
 * That is also what keeps the dpd verdict honest -- without it, a block that quietly
 * kept its power would pass as a block that was rebuilt.
 *
 * What is asserted after the transition is behavioural as well as register-level:
 * that the output is really toggling. An SCTimer's counter runs off the bus clock,
 * which the board selects and this driver does not, so a register image that came
 * back perfectly while the clock did not would look restored and still drive nothing.
 */

#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include <pm_test/lowpower.h>
#include <pm_test/state.h>

#include "sctimer.h"

ZTEST(sctimer_pm, test_sweep_across_transition)
{
	SCTIMER_FOREACH_CASE(c) {
		struct sctimer_image configured;
		struct sctimer_image after;
		bool gate_after;
		bool witness_after = false;

		/* No runtime PM in these layers, so the sweep is the only thing that will
		 * suspend the device.
		 */
		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		zassert_ok(sctimer_configure(c), "%s: pwm_set_cycles() failed before the "
			   "transition", c->dev->name);
		zassert_true(sctimer_output_toggles(c),
			     "%s: the output is not toggling before the transition",
			     c->dev->name);

		sctimer_witness_write(c);
		sctimer_image_read(c, &configured);
		sctimer_report_hw(c, "before");

		pm_test_enter_transition();

		/* Sampled before the console is revived: reviving it takes time, and the
		 * question is what the block looked like the moment the sweep handed it
		 * back.
		 */
		gate_after = sctimer_gate_is_open(c);
		if (gate_after) {
			sctimer_image_read(c, &after);
			witness_after = sctimer_witness_survives(c);
		}

		pm_test_console_resume(PM_TEST_CONSOLE_REINIT_PINMUX);
		TC_PRINT("resumed from %s\n", PM_TEST_TRANSITION_NAME);
		sctimer_report_hw(c, "after-resume");

		zassert_pm_state(c->dev, PM_DEVICE_STATE_ACTIVE);

		if (!sctimer_gate_observable(c)) {
			TC_PRINT("%s: clock gate not observable, register claims skipped\n",
				 c->dev->name);
		} else {
			/*
			 * The gate first: without it the register comparisons below would
			 * be reading an unclocked block, and "restored" would be whatever
			 * the bus returns.
			 */
			zassert_true(gate_after,
				     "%s: the clock gate was not re-opened across the "
				     "transition", c->dev->name);

			/* Both layers owe the channel back, by different routes. */
			zassert_true(sctimer_image_equal(&after, &configured),
				     "%s: the channel did not come back across %s: "
				     "MATCHREL0=0x%08x MATCHREL1=0x%08x SET=0x%08x "
				     "CLR=0x%08x, want 0x%08x/0x%08x/0x%08x/0x%08x",
				     c->dev->name, PM_TEST_TRANSITION_NAME,
				     after.matchrel_period, after.matchrel_pulse,
				     after.out_set, after.out_clr, configured.matchrel_period,
				     configured.matchrel_pulse, configured.out_set,
				     configured.out_clr);

#if defined(CONFIG_PM_TEST_LAYER_DPD)
			/*
			 * And the witness says the image above is a replay and not a block
			 * that was never disturbed.
			 */
			zassert_false(witness_after,
				      "%s: the witness survived %s, so the block kept its "
				      "power and TURN_ON was never asked to rebuild it",
				      c->dev->name, PM_TEST_TRANSITION_NAME);
#else
			zassert_true(witness_after,
				     "%s: the witness is gone after %s, so the block lost "
				     "power in a state that was supposed to keep it",
				     c->dev->name, PM_TEST_TRANSITION_NAME);
#endif
		}

		/* And the block has to drive, not just read back right. */
		zassert_true(sctimer_output_toggles(c),
			     "%s: the output is not toggling after %s", c->dev->name,
			     PM_TEST_TRANSITION_NAME);
	}
}
