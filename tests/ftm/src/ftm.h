/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief What every layer of the FTM case needs from the hardware.
 *
 * Three things are observed.
 *
 * The peripheral clock gate, read out of the PCC rather than out of an FTM
 * register: an FTM register only answers while the peripheral is clocked, and whether
 * it is clocked is the question. Gating that clock is the whole of what SUSPEND
 * does for power.
 *
 * The register image of a running PWM: MOD holds the period and
 * CONTROLS[ch].CnV the pulse width, and nothing else in the peripheral carries them.
 * This is what tells a restored FTM from one that came back from reset. Both read
 * zero out of reset and non-zero once a consumer has asked for a waveform.
 *
 * CNT advancing, which is the control: an FTM takes its counter clock through a
 * source and a prescaler that the board devicetree picks rather than this driver,
 * so a register image that came back perfectly while the clock source did not
 * would read as restored and still not count.
 *
 * What is not observed is the waveform on the pin. An FTM has no completion event
 * a test could wait on, so proving the output really toggles needs an external
 * loopback -- that is what tests/drivers/pwm/pwm_loopback is for. See README.md.
 */

#ifndef FTM_PM_H_
#define FTM_PM_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>

#if !DT_HAS_COMPAT_STATUS_OKAY(nxp_ftm_pwm)
#error "No nxp,ftm-pwm node is enabled on this board"
#endif

#if !defined(CONFIG_PWM_MCUX_FTM)
#error "CONFIG_PWM_MCUX_FTM is not enabled, so there is no driver to test"
#endif

/**
 * @brief The channel every test drives.
 *
 * Channel 0 exists on every instance and is pin-muxed on both board targets this
 * case covers, so the register image below corresponds to a pin that is really
 * driven even though no test asserts on its level.
 */
#define FTM_PWM_CHANNEL 0U

/**
 * @brief The period and pulse every test asks for, in counter ticks.
 *
 * Asked for in ticks rather than nanoseconds so that the register image is the
 * same on both boards: the counter clock differs between them, and MOD has to stay
 * inside 16 bits whatever it is.
 */
#define FTM_PWM_PERIOD_CYCLES 10000U
#define FTM_PWM_PULSE_CYCLES  5000U

/** How many enabled nxp,ftm-pwm instances this build covers. */
#define FTM_PM_COUNT_ONE(node_id) +1
#define FTM_CASE_COUNT (0 DT_FOREACH_STATUS_OKAY(nxp_ftm_pwm, FTM_PM_COUNT_ONE))

/** One FTM instance under test. */
struct ftm_case {
	const struct device *dev;
	/** Register block, for the registers a running PWM lives in. */
	uintptr_t base;
};

extern const struct ftm_case ftm_cases[FTM_CASE_COUNT];

/** @brief Iterate the instances. */
#define FTM_FOREACH_CASE(c)                                                                        \
	for (const struct ftm_case *c = &ftm_cases[0];                                             \
	     c < &ftm_cases[FTM_CASE_COUNT]; c++)

/**
 * @brief True when this SoC HAL names a clock gate for @p c.
 *
 * An instance whose gate cannot be named is reported and asserted nothing about,
 * rather than passing quietly.
 */
bool ftm_gate_observable(const struct ftm_case *c);

/** @brief True while the instance peripheral clock gate is open. */
bool ftm_gate_is_open(const struct ftm_case *c);

/** @brief Open the gate behind the driver back, so a clobber can be written. */
void ftm_gate_open(const struct ftm_case *c);

/** @brief Close it again, leaving the peripheral as a suspend left it. */
void ftm_gate_close(const struct ftm_case *c);

/**
 * @brief The registers a running PWM lives in.
 *
 * Only meaningful while the peripheral is clocked; every caller here reads it with the
 * gate open.
 */
struct ftm_image {
	uint32_t mod;
	uint32_t cnv;
	uint32_t sc;
};

/** @brief Sample @ref ftm_image. */
void ftm_image_read(const struct ftm_case *c, struct ftm_image *img);

/** @brief True when two images are the same. */
bool ftm_image_equal(const struct ftm_image *a, const struct ftm_image *b);

/**
 * @brief Overwrite the running-PWM registers with a cleared image.
 *
 * What stands in for a power cycle in a layer where nothing really cut power, so
 * that a restore has something to put back. Takes care of the clock gate itself
 * and leaves it as it found it.
 *
 * @param c         instance to clobber
 * @param clobbered read back after the write, so a caller can prove the clobber
 *                  took
 */
void ftm_clobber(const struct ftm_case *c, struct ftm_image *clobbered);

/** @brief True while SC[CLKS] says the counter is clocked. */
bool ftm_running(const struct ftm_case *c);

/** @brief True when two CNT reads a short wait apart differ. */
bool ftm_advances(const struct ftm_case *c);

/**
 * @brief Ask for the waveform every test uses.
 *
 * @return 0, or negative errno from pwm_set_cycles().
 */
int ftm_set_pwm(const struct ftm_case *c);

/**
 * @brief Hand the channel back, so the driver has a reason to drop its lock.
 *
 * A period of 0 is what the driver takes as "drive nothing out". It is the only
 * way through the PWM API to undo @ref ftm_set_pwm while the device stays active,
 * which is what a counter gets from counter_stop().
 *
 * @return 0, or negative errno from pwm_set_cycles().
 */
int ftm_release_pwm(const struct ftm_case *c);

/**
 * @brief Ask for the waveform and check that the counter is actually running.
 *
 * @return 0, negative errno from the PWM API, or -ESTALE when CNT did not move.
 */
int ftm_start_and_verify(const struct ftm_case *c);

/** @brief Print what the devicetree asked for, once, from the suite setup. */
void ftm_report_configuration(void);

/**
 * @brief Print the gate and, when the gate is open, the register image.
 *
 * Safe to call with the gate closed: it prints the gate and says the registers
 * were not read rather than faulting on an unclocked peripheral.
 *
 * @param when short label for where in the sequence this is, e.g. "before".
 */
void ftm_report_hw(const struct ftm_case *c, const char *when);

#endif /* FTM_PM_H_ */
