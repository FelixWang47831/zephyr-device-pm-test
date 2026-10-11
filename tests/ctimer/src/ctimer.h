/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief What every layer of the CTIMER case needs from the hardware.
 *
 * Three things are observed.
 *
 * The block's AHB clock gate, read out of the clock controller rather than out of
 * a CTIMER register: a CTIMER register only answers while the block is clocked,
 * and whether it is clocked is the question. Gating that clock is the whole of
 * what SUSPEND does for power, and from the counter API alone a stopped counter
 * and a suspended one look identical.
 *
 * The register image of an armed alarm: MR[0] holds the match value and MCR its
 * interrupt enable, and nothing else in the block carries them. A suspend keeps
 * the block powered, so the image it hands back has to be the one it was given.
 *
 * A counter that advances, which is the control: it is what makes "the gate is
 * open" mean the peripheral works rather than that a bit is set.
 *
 * Across a power cycle the assertion is behavioural instead: an alarm armed fresh
 * after the round trip has to fire. Re-arming is the consumer's, because the
 * counter API gives it stop() and cancel_alarm() and a count value that no
 * restore can make meaningful again. What the driver owes is a device that can be
 * armed, and it cannot be unless TURN_ON re-ran init and re-connected the
 * interrupt.
 */

#ifndef CTIMER_PM_H_
#define CTIMER_PM_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/counter.h>

#if !DT_HAS_COMPAT_STATUS_OKAY(nxp_lpc_ctimer)
#error "No nxp,lpc-ctimer node is enabled on this board"
#endif

#if !defined(CONFIG_COUNTER_MCUX_CTIMER)
#error "CONFIG_COUNTER_MCUX_CTIMER is not enabled, so there is no driver to test"
#endif

/** The alarm channel every test arms. Channel 0 exists on every instance. */
#define CTIMER_ALARM_CHANNEL 0U

/** How many enabled nxp,lpc-ctimer instances this build covers. */
#define CTIMER_PM_COUNT_ONE(node_id) +1
#define CTIMER_CASE_COUNT (0 DT_FOREACH_STATUS_OKAY(nxp_lpc_ctimer, CTIMER_PM_COUNT_ONE))

/** One CTIMER instance under test. */
struct ctimer_case {
	const struct device *dev;
	/** Instance number, from the syscon clock cell; names the clock gate. */
	uint8_t index;
	/** Register block, for the two registers an armed alarm lives in. */
	uintptr_t base;
};

extern const struct ctimer_case ctimer_cases[CTIMER_CASE_COUNT];

/** @brief Iterate the instances. */
#define CTIMER_FOREACH_CASE(c)                                                                     \
	for (const struct ctimer_case *c = &ctimer_cases[0];                                       \
	     c < &ctimer_cases[CTIMER_CASE_COUNT]; c++)

/**
 * @brief True when this SoC's HAL names a clock gate for @p c.
 *
 * An instance whose gate cannot be named is reported and asserted nothing about,
 * rather than passing quietly.
 */
bool ctimer_gate_observable(const struct ctimer_case *c);

/** @brief True while the instance's AHB clock gate is open. */
bool ctimer_gate_is_open(const struct ctimer_case *c);

/** @brief Open the gate behind the driver's back, so a clobber can be written. */
void ctimer_gate_open(const struct ctimer_case *c);

/** @brief Close it again, leaving the block as a suspend left it. */
void ctimer_gate_close(const struct ctimer_case *c);

/**
 * @brief The registers an armed alarm lives in.
 *
 * Only meaningful while the block is clocked; every caller here reads it with the
 * gate open.
 */
struct ctimer_image {
	uint32_t mcr;
	uint32_t mr;
	uint32_t tcr;
};

/** @brief Sample @ref ctimer_image. */
void ctimer_image_read(const struct ctimer_case *c, struct ctimer_image *img);

/** @brief True when two images are the same. */
bool ctimer_image_equal(const struct ctimer_image *a, const struct ctimer_image *b);

/**
 * @brief Overwrite the armed-alarm registers with a cleared image.
 *
 * What stands in for a power cycle in a layer where nothing really cut power, so
 * that a restore has something to put back. Takes care of the clock gate itself and
 * leaves it as it found it.
 *
 * @param c         instance to clobber
 * @param clobbered read back after the write, so a caller can prove the clobber took
 */
void ctimer_clobber(const struct ctimer_case *c, struct ctimer_image *clobbered);

/** @brief True while TCR[CEN] says the counter is running. */
bool ctimer_running(const struct ctimer_case *c);

/**
 * @brief Start the counter and check that it is actually counting.
 *
 * @return 0 when two reads taken a short wait apart differ, negative errno from
 *         the counter API, or -ESTALE when the counter did not move.
 */
int ctimer_start_and_verify(const struct ctimer_case *c);

/** @brief counter_stop(). */
int ctimer_stop(const struct ctimer_case *c);

/** @brief True when two reads a short wait apart differ. */
bool ctimer_advances(const struct ctimer_case *c);

/**
 * @brief Arm the alarm on @ref CTIMER_ALARM_CHANNEL, @p delay_ms out.
 *
 * Clears the fired flag first. The alarm is relative, so it is measured from the
 * counter's current value.
 *
 * @return 0, or negative errno from counter_set_alarm().
 */
int ctimer_arm_alarm(const struct ctimer_case *c, uint32_t delay_ms);

/** @brief Cancel it again, whether or not it fired. */
int ctimer_cancel_alarm(const struct ctimer_case *c);

/** @brief True once the alarm callback has run. */
bool ctimer_alarm_fired(const struct ctimer_case *c);

/**
 * @brief Wait up to @p timeout_ms for the alarm to fire.
 *
 * @return true if it fired within the timeout.
 */
bool ctimer_alarm_wait(const struct ctimer_case *c, uint32_t timeout_ms);

/** @brief Print what the devicetree asked for, once, from the suite setup. */
void ctimer_report_configuration(void);

/**
 * @brief Print the gate and, when the gate is open, the register image.
 *
 * Safe to call with the gate closed: it prints the gate and says the registers
 * were not read rather than faulting on an unclocked block.
 *
 * @param when short label for where in the sequence this is, e.g. "before".
 */
void ctimer_report_hw(const struct ctimer_case *c, const char *when);

#endif /* CTIMER_PM_H_ */
