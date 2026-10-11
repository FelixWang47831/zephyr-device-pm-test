/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief What the SCTimer layers share: the instance table and the register view.
 *
 * The contract this case holds the driver to, for a block that *holds* its output:
 *
 * - The consumer owns the power management reference. A PWM output drives until the
 *   consumer changes it, and there is no completion event a put() could hang on, so
 *   the driver takes no reference of its own and an API call that arrives while the
 *   device is not active is refused rather than silently dropped on the floor.
 * - A suspend halts the counter, parks every configured output at its inactive level
 *   and gates the clock, but leaves the channel programming in the registers.
 * - A resume re-opens the gate and restarts the counter for a channel the consumer
 *   had configured, without the consumer asking again.
 * - A turn-on rebuilds the block from scratch and replays the channel the consumer
 *   had configured, so a power cycle is invisible to it.
 *
 * This case makes no claim about the pad itself. Whether the pin is still driving
 * the level the SCTimer holds depends on the board's pinctrl sleep state and on what
 * the SoC does to the mux across a transition, neither of which is readable from
 * here. Everything below is read out of the SCT register block and the clock gate.
 */

#ifndef ZEPHYR_TESTS_SCTIMER_SCTIMER_H_
#define ZEPHYR_TESTS_SCTIMER_SCTIMER_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/util.h>

#if !DT_HAS_COMPAT_STATUS_OKAY(nxp_sctimer_pwm)
#error "no enabled nxp,sctimer-pwm node: this board target cannot run this case"
#endif

#if !defined(CONFIG_PWM_MCUX_SCTIMER)
#error "CONFIG_PWM_MCUX_SCTIMER is not enabled: this case tests that driver"
#endif

/*
 * Channel 0, because frdm_mcxn947 is the only board target that routes an SCTimer
 * output and routes exactly one: SCT0_OUT0. A period in bus-clock cycles rather
 * than nanoseconds, so nothing here depends on the clock the board attached.
 */
#define SCTIMER_PWM_CHANNEL 0U
#define SCTIMER_PWM_PERIOD  1500U
#define SCTIMER_PWM_PULSE   750U

#define SCTIMER_PM_COUNT_ONE(node_id) 1 +

/** Number of enabled nxp,sctimer-pwm instances this build covers. */
#define SCTIMER_CASE_COUNT (DT_FOREACH_STATUS_OKAY(nxp_sctimer_pwm, SCTIMER_PM_COUNT_ONE) 0)

/** One SCTimer instance under test. */
struct sctimer_case {
	const struct device *dev;
	uintptr_t base;
};

extern const struct sctimer_case sctimer_cases[SCTIMER_CASE_COUNT];

/** Iterate over every instance this build covers. */
#define SCTIMER_FOREACH_CASE(c)                                                                    \
	for (const struct sctimer_case *c = &sctimer_cases[0];                                     \
	     c < &sctimer_cases[SCTIMER_CASE_COUNT]; c++)

/**
 * @brief The register state a configured channel leaves behind.
 *
 * The period and pulse reload values and the output set/clear events: between them,
 * everything the driver writes for one channel, and all of it 0 after a reset.
 */
struct sctimer_image {
	uint32_t matchrel_period;
	uint32_t matchrel_pulse;
	uint32_t out_set;
	uint32_t out_clr;
};

/** Whether the clock gate can be read on the SoC being built. */
bool sctimer_gate_observable(const struct sctimer_case *c);

/** Whether the SCTimer clock gate is open, read out of the clock controller. */
bool sctimer_gate_is_open(const struct sctimer_case *c);

/** Open the clock gate behind the driver's back, so a register can be reached. */
void sctimer_gate_open(const struct sctimer_case *c);

/** Close the clock gate behind the driver's back. */
void sctimer_gate_close(const struct sctimer_case *c);

/** Read the register image of the channel under test. */
void sctimer_image_read(const struct sctimer_case *c, struct sctimer_image *img);

/** Whether two images are the same. */
bool sctimer_image_equal(const struct sctimer_image *a, const struct sctimer_image *b);

/** Whether an image is all zeroes, which is what a reset leaves. */
bool sctimer_image_is_clear(const struct sctimer_image *img);

/** Zero the channel's registers by hand, and report what that left. */
void sctimer_clobber(const struct sctimer_case *c, struct sctimer_image *clobbered);

/** What the block looks like while it is gated, read by borrowing the clock. */
struct sctimer_peek {
	struct sctimer_image image;
	bool running;
	bool output_high;
};

/**
 * @brief Read a gated block by borrowing its clock, then hand the clock back.
 *
 * An SCT register only answers while the block is clocked, so what a suspend left in
 * there cannot be read without re-opening the gate. This opens it, reads, and closes
 * it again, leaving the hardware as it was found. The gate itself must be sampled
 * with sctimer_gate_is_open() before this is called -- afterwards it says nothing.
 */
void sctimer_peek_gated(const struct sctimer_case *c, struct sctimer_peek *p);


/**
 * @brief Write a value no driver action rewrites, as a witness for lost power.
 *
 * The driver replays the channel the consumer configured, so a restored image looks
 * the same whether the block kept its power or was rebuilt from the devicetree. This
 * writes the last match reload register, which the driver never touches: it survives
 * a suspend and comes back at its reset value from a power cycle.
 */
void sctimer_witness_write(const struct sctimer_case *c);

/** Whether the witness value is still in the register. */
bool sctimer_witness_survives(const struct sctimer_case *c);

/** Whether the counter is running, read out of CTRL. */
bool sctimer_running(const struct sctimer_case *c);

/** Whether the output under test is currently high, read out of OUTPUT. */
bool sctimer_output_is_high(const struct sctimer_case *c);

/** Whether the counter is advancing, from two reads of COUNT. */
bool sctimer_advances(const struct sctimer_case *c);

/** Whether the output is actually toggling, from repeated reads of OUTPUT. */
bool sctimer_output_toggles(const struct sctimer_case *c);

/** Configure the channel under test. Returns what pwm_set_cycles() returned. */
int sctimer_configure(const struct sctimer_case *c);

/** Configure the channel and check the counter really started. */
int sctimer_configure_and_verify(const struct sctimer_case *c);

/** Print what this build covers, once, from the suite setup. */
void sctimer_report_configuration(void);

/** Print the gate and the register image, labelled, without asserting anything. */
void sctimer_report_hw(const struct sctimer_case *c, const char *when);

#endif /* ZEPHYR_TESTS_SCTIMER_SCTIMER_H_ */
