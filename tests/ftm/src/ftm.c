/*
 * SPDX-FileCopyrightText: Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The FTM wiring, and the hardware truths this case asserts on.
 */

#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/tc_util.h>

#include <fsl_clock.h>
#include <fsl_ftm.h>
#include <soc.h>

#include "ftm.h"

#define FTM_ENTRY(node_id)                                                                         \
	{                                                                                          \
		.dev = DEVICE_DT_GET(node_id),                                                     \
		.base = (uintptr_t)DT_REG_ADDR(node_id),                                           \
	},

const struct ftm_case ftm_cases[FTM_CASE_COUNT] = {
	DT_FOREACH_STATUS_OKAY(nxp_ftm_pwm, FTM_ENTRY)
};

static FTM_Type *ftm_base(const struct ftm_case *c)
{
	return (FTM_Type *)c->base;
}

/*
 * The instance number, from the register address rather than from the devicetree
 * clocks cell. The first clocks entry of an FTM node names the counter clock
 * source, not the gate -- frdm_ke17z points it at SCG and frdm_mcxe247 at PCC --
 * so it cannot name the gate for both. FTM_BASE_PTRS is the HAL own list and is
 * right on every part it serves.
 */
static bool ftm_instance_of(const struct ftm_case *c, uint32_t *instance)
{
	FTM_Type *const bases[] = FTM_BASE_PTRS;

	for (uint32_t i = 0U; i < ARRAY_SIZE(bases); i++) {
		if (bases[i] == ftm_base(c)) {
			*instance = i;
			return true;
		}
	}

	return false;
}

/*
 * kCLOCK_Ftm<n> is the absolute address of the PCC register that holds this
 * instance gate, which is how the HAL CLOCK_EnableClock() and
 * CLOCK_DisableClock() reach it. Guarded by the HAL instance count rather than
 * by the devicetree: MKE17Z stops at FTM2 and naming kCLOCK_Ftm3 there does not
 * compile, while MCXE24x has eight.
 */
static bool ftm_gate_of(const struct ftm_case *c, clock_ip_name_t *gate)
{
	uint32_t instance;

	if (!ftm_instance_of(c, &instance)) {
		return false;
	}

	switch (instance) {
	case 0:
		*gate = kCLOCK_Ftm0;
		break;
#if FSL_FEATURE_SOC_FTM_COUNT > 1
	case 1:
		*gate = kCLOCK_Ftm1;
		break;
#endif
#if FSL_FEATURE_SOC_FTM_COUNT > 2
	case 2:
		*gate = kCLOCK_Ftm2;
		break;
#endif
#if FSL_FEATURE_SOC_FTM_COUNT > 3
	case 3:
		*gate = kCLOCK_Ftm3;
		break;
#endif
#if FSL_FEATURE_SOC_FTM_COUNT > 4
	case 4:
		*gate = kCLOCK_Ftm4;
		break;
#endif
#if FSL_FEATURE_SOC_FTM_COUNT > 5
	case 5:
		*gate = kCLOCK_Ftm5;
		break;
#endif
#if FSL_FEATURE_SOC_FTM_COUNT > 6
	case 6:
		*gate = kCLOCK_Ftm6;
		break;
#endif
#if FSL_FEATURE_SOC_FTM_COUNT > 7
	case 7:
		*gate = kCLOCK_Ftm7;
		break;
#endif
	default:
		return false;
	}

	return true;
}

bool ftm_gate_observable(const struct ftm_case *c)
{
	clock_ip_name_t gate;

	return ftm_gate_of(c, &gate);
}

bool ftm_gate_is_open(const struct ftm_case *c)
{
	clock_ip_name_t gate;

	if (!ftm_gate_of(c, &gate)) {
		return false;
	}

	/* The gate enum is the register address, and unlike the LPC-style write-only
	 * SET/CLR aliases this register is readable, so the gate can be read back
	 * directly.
	 */
	return (*(const volatile uint32_t *)(uintptr_t)gate & PCC_CLKCFG_CGC_MASK) != 0U;
}

void ftm_gate_open(const struct ftm_case *c)
{
	clock_ip_name_t gate;

	if (ftm_gate_of(c, &gate)) {
		CLOCK_EnableClock(gate);
	}
}

void ftm_gate_close(const struct ftm_case *c)
{
	clock_ip_name_t gate;

	if (ftm_gate_of(c, &gate)) {
		CLOCK_DisableClock(gate);
	}
}

void ftm_image_read(const struct ftm_case *c, struct ftm_image *img)
{
	FTM_Type *base = ftm_base(c);

	img->mod = base->MOD;
	img->cnv = base->CONTROLS[FTM_PWM_CHANNEL].CnV;
	img->sc = base->SC;
}

bool ftm_image_equal(const struct ftm_image *a, const struct ftm_image *b)
{
	return (a->mod == b->mod) && (a->cnv == b->cnv) && (a->sc == b->sc);
}

void ftm_clobber(const struct ftm_case *c, struct ftm_image *clobbered)
{
	FTM_Type *base = ftm_base(c);
	bool was_open = ftm_gate_is_open(c);

	/* The registers cannot be written while the peripheral is unclocked, and this is
	 * called from a layer where the driver has already gated it.
	 */
	ftm_gate_open(c);

	/* SC last: clearing CLKS is what stops the counter, and the writes above want
	 * a clocked peripheral.
	 */
	base->MOD = 0U;
	base->CONTROLS[FTM_PWM_CHANNEL].CnV = 0U;
	base->SC = 0U;

	ftm_image_read(c, clobbered);

	if (!was_open) {
		ftm_gate_close(c);
	}
}

bool ftm_running(const struct ftm_case *c)
{
	return (ftm_base(c)->SC & FTM_SC_CLKS_MASK) != 0U;
}

bool ftm_advances(const struct ftm_case *c)
{
	uint32_t first = ftm_base(c)->CNT;
	uint32_t second;

	/*
	 * Busy-waited rather than slept: at the slowest counter clock either board
	 * attaches this is still several ticks, and a sleep would let the idle thread
	 * enter a power state in the layers that have one.
	 */
	k_busy_wait(2000);

	second = ftm_base(c)->CNT;

	return first != second;
}

int ftm_set_pwm(const struct ftm_case *c)
{
	return pwm_set_cycles(c->dev, FTM_PWM_CHANNEL, FTM_PWM_PERIOD_CYCLES,
			      FTM_PWM_PULSE_CYCLES, 0);
}

int ftm_release_pwm(const struct ftm_case *c)
{
	return pwm_set_cycles(c->dev, FTM_PWM_CHANNEL, 0U, 0U, 0);
}

int ftm_start_and_verify(const struct ftm_case *c)
{
	int err = ftm_set_pwm(c);

	if (err != 0) {
		return err;
	}

	return ftm_advances(c) ? 0 : -ESTALE;
}

void ftm_report_configuration(void)
{
	TC_PRINT("%u FTM instance(s) under test\n", (unsigned int)FTM_CASE_COUNT);

	FTM_FOREACH_CASE(c) {
		uint32_t instance = 0U;
		uint64_t cycles = 0U;

		(void)ftm_instance_of(c, &instance);
		(void)pwm_get_cycles_per_sec(c->dev, FTM_PWM_CHANNEL, &cycles);

		TC_PRINT("  %s: FTM%u at %p, %u Hz counter clock, channel %u, gate %s\n",
			 c->dev->name, (unsigned int)instance, (void *)c->base,
			 (unsigned int)cycles, (unsigned int)FTM_PWM_CHANNEL,
			 ftm_gate_observable(c) ? "observable"
						      : "NOT observable on this SoC");
	}
}

void ftm_report_hw(const struct ftm_case *c, const char *when)
{
	if (!ftm_gate_observable(c)) {
		TC_PRINT("hw(%s, %s) gate not observable on this SoC\n", when, c->dev->name);
		return;
	}

	if (!ftm_gate_is_open(c)) {
		/* No register read: an unclocked peripheral does not answer. */
		TC_PRINT("hw(%s, %s) gate=0, registers not read\n", when, c->dev->name);
		return;
	}

	struct ftm_image img;

	ftm_image_read(c, &img);
	TC_PRINT("hw(%s, %s) gate=1 SC=0x%08x MOD=0x%08x CnV%u=0x%08x CNT=0x%08x\n", when,
		 c->dev->name, img.sc, img.mod, (unsigned int)FTM_PWM_CHANNEL, img.cnv,
		 ((FTM_Type *)c->base)->CNT);
}
