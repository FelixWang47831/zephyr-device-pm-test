/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The CTIMER wiring, and the hardware truths this case asserts on.
 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/tc_util.h>

#include <zephyr/dt-bindings/clock/mcux_lpc_syscon_clock.h>
#include <fsl_clock.h>
#include <soc.h>

#include "ctimer.h"

/*
 * Every instance takes its clock from the syscon node with a single MCUX_CTIMERn
 * cell, so the instance number is the distance from MCUX_CTIMER0_CLK. That is also
 * what names the clock gate below, so a part wiring this driver to some other clock
 * controller would need a different mapping and is refused here rather than read
 * out of the wrong register.
 */
#define CTIMER_ASSERT_SYSCON_CLOCK(node_id)                                                        \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CLOCKS_CTLR(node_id), nxp_lpc_syscon),                  \
		     "this case reads the CTIMER clock gate out of the MCX clock controller and "   \
		     "only fits SoCs whose CTIMER clocks come from nxp,lpc-syscon");

DT_FOREACH_STATUS_OKAY(nxp_lpc_ctimer, CTIMER_ASSERT_SYSCON_CLOCK)

#define CTIMER_ENTRY(node_id)                                                                      \
	{                                                                                          \
		.dev = DEVICE_DT_GET(node_id),                                                     \
		.index = (uint8_t)(DT_CLOCKS_CELL(node_id, name) - MCUX_CTIMER0_CLK),              \
		.base = (uintptr_t)DT_REG_ADDR(node_id),                                           \
	},

const struct ctimer_case ctimer_cases[CTIMER_CASE_COUNT] = {
	DT_FOREACH_STATUS_OKAY(nxp_lpc_ctimer, CTIMER_ENTRY)
};

/* Set from the alarm callback; one slot per instance, indexed by ctimer_cases. */
static volatile bool ctimer_fired[CTIMER_CASE_COUNT];

static CTIMER_Type *ctimer_base(const struct ctimer_case *c)
{
	return (CTIMER_Type *)c->base;
}

static uint8_t ctimer_slot(const struct ctimer_case *c)
{
	return (uint8_t)(c - &ctimer_cases[0]);
}

static bool ctimer_gate_of(uint8_t index, clock_ip_name_t *gate)
{
	/*
	 * Guarded by the HAL's instance count, not by the devicetree: MCXA153 and
	 * MCXA344 stop at CTIMER2 and have no gate constant beyond it, so naming
	 * kCLOCK_GateCTIMER3 there does not compile. Same guard as the syscon clock
	 * driver's own CTIMER cases in clock_control_mcux_syscon.c.
	 */
#if defined(CONFIG_SOC_FAMILY_MCXA)
	switch (index) {
	case 0:
		*gate = kCLOCK_GateCTIMER0;
		break;
#if FSL_FEATURE_SOC_CTIMER_COUNT > 1
	case 1:
		*gate = kCLOCK_GateCTIMER1;
		break;
#endif
#if FSL_FEATURE_SOC_CTIMER_COUNT > 2
	case 2:
		*gate = kCLOCK_GateCTIMER2;
		break;
#endif
#if FSL_FEATURE_SOC_CTIMER_COUNT > 3
	case 3:
		*gate = kCLOCK_GateCTIMER3;
		break;
#endif
#if FSL_FEATURE_SOC_CTIMER_COUNT > 4
	case 4:
		*gate = kCLOCK_GateCTIMER4;
		break;
#endif
	default:
		return false;
	}

	return *gate != kCLOCK_GateNotAvail;
#else
	switch (index) {
	case 0:
		*gate = kCLOCK_Timer0;
		break;
#if FSL_FEATURE_SOC_CTIMER_COUNT > 1
	case 1:
		*gate = kCLOCK_Timer1;
		break;
#endif
#if FSL_FEATURE_SOC_CTIMER_COUNT > 2
	case 2:
		*gate = kCLOCK_Timer2;
		break;
#endif
#if FSL_FEATURE_SOC_CTIMER_COUNT > 3
	case 3:
		*gate = kCLOCK_Timer3;
		break;
#endif
#if FSL_FEATURE_SOC_CTIMER_COUNT > 4
	case 4:
		*gate = kCLOCK_Timer4;
		break;
#endif
	default:
		return false;
	}

	return *gate != kCLOCK_None;
#endif
}

bool ctimer_gate_observable(const struct ctimer_case *c)
{
	clock_ip_name_t gate;

	return ctimer_gate_of(c->index, &gate);
}

/*
 * CLOCK_EnableClock()/CLOCK_DisableClock() write through the write-only SET/CLR
 * aliases, so the readable register is the one at the head of each group:
 * SYSCON->AHBCLKCTRL0..3 on MCXN (contiguous from 0x200), MRCC0->MRCC_GLB_CCn on
 * MCXA (group stride 0x10, which is what the HAL's CLK_GATE_REG_OFFSET() encodes).
 */
bool ctimer_gate_is_open(const struct ctimer_case *c)
{
	clock_ip_name_t gate;

	if (!ctimer_gate_of(c->index, &gate)) {
		return false;
	}

#if defined(CONFIG_SOC_FAMILY_MCXA)
	const volatile uint32_t *reg =
		(const volatile uint32_t *)((uintptr_t)&MRCC0->MRCC_GLB_CC0 +
					    CLK_GATE_REG_OFFSET(gate));

	return (*reg & BIT(CLK_GATE_BIT_SHIFT(gate))) != 0U;
#else
	const volatile uint32_t *reg = &(&SYSCON->AHBCLKCTRL0)[CLK_GATE_ABSTRACT_REG_OFFSET(gate)];

	return (*reg & BIT(CLK_GATE_ABSTRACT_BITS_SHIFT(gate))) != 0U;
#endif
}

void ctimer_gate_open(const struct ctimer_case *c)
{
	clock_ip_name_t gate;

	if (ctimer_gate_of(c->index, &gate)) {
		CLOCK_EnableClock(gate);
	}
}

void ctimer_gate_close(const struct ctimer_case *c)
{
	clock_ip_name_t gate;

	if (ctimer_gate_of(c->index, &gate)) {
		CLOCK_DisableClock(gate);
	}
}

void ctimer_image_read(const struct ctimer_case *c, struct ctimer_image *img)
{
	CTIMER_Type *base = ctimer_base(c);

	img->mcr = base->MCR;
	img->mr = base->MR[CTIMER_ALARM_CHANNEL];
	img->tcr = base->TCR;
}

bool ctimer_image_equal(const struct ctimer_image *a, const struct ctimer_image *b)
{
	return (a->mcr == b->mcr) && (a->mr == b->mr) && (a->tcr == b->tcr);
}

void ctimer_clobber(const struct ctimer_case *c, struct ctimer_image *clobbered)
{
	CTIMER_Type *base = ctimer_base(c);
	bool was_open = ctimer_gate_is_open(c);

	/* The registers cannot be written while the block is unclocked, and this is
	 * called from a layer where the driver has already gated it.
	 */
	ctimer_gate_open(c);

	base->TCR = 0U;
	base->MCR = 0U;
	base->MR[CTIMER_ALARM_CHANNEL] = 0U;

	ctimer_image_read(c, clobbered);

	if (!was_open) {
		ctimer_gate_close(c);
	}
}

bool ctimer_running(const struct ctimer_case *c)
{
	return (ctimer_base(c)->TCR & CTIMER_TCR_CEN_MASK) != 0U;
}

bool ctimer_advances(const struct ctimer_case *c)
{
	uint32_t first;
	uint32_t second;

	if (counter_get_value(c->dev, &first) != 0) {
		return false;
	}

	/*
	 * Busy-waited rather than slept: at the slowest CTIMER clock any board here
	 * attaches this is still hundreds of ticks, and a sleep would let the idle
	 * thread enter a power state in the layers that have one.
	 */
	k_busy_wait(200);

	if (counter_get_value(c->dev, &second) != 0) {
		return false;
	}

	return first != second;
}

int ctimer_start_and_verify(const struct ctimer_case *c)
{
	int err = counter_start(c->dev);

	if (err != 0) {
		return err;
	}

	return ctimer_advances(c) ? 0 : -ESTALE;
}

int ctimer_stop(const struct ctimer_case *c)
{
	return counter_stop(c->dev);
}

static void ctimer_alarm_callback(const struct device *dev, uint8_t chan_id, uint32_t ticks,
				  void *user_data)
{
	const struct ctimer_case *c = user_data;

	ARG_UNUSED(dev);
	ARG_UNUSED(chan_id);
	ARG_UNUSED(ticks);

	ctimer_fired[ctimer_slot(c)] = true;
}

int ctimer_arm_alarm(const struct ctimer_case *c, uint32_t delay_ms)
{
	uint32_t freq = counter_get_frequency(c->dev);
	struct counter_alarm_cfg cfg = {
		.callback = ctimer_alarm_callback,
		.user_data = (void *)c,
		.flags = 0,
	};

	if (freq == 0U) {
		return -EIO;
	}

	cfg.ticks = (uint32_t)(((uint64_t)freq * delay_ms) / 1000U);
	if (cfg.ticks == 0U) {
		return -EINVAL;
	}

	ctimer_fired[ctimer_slot(c)] = false;

	return counter_set_channel_alarm(c->dev, CTIMER_ALARM_CHANNEL, &cfg);
}

int ctimer_cancel_alarm(const struct ctimer_case *c)
{
	return counter_cancel_channel_alarm(c->dev, CTIMER_ALARM_CHANNEL);
}

bool ctimer_alarm_fired(const struct ctimer_case *c)
{
	return ctimer_fired[ctimer_slot(c)];
}

bool ctimer_alarm_wait(const struct ctimer_case *c, uint32_t timeout_ms)
{
	for (uint32_t waited = 0U; waited < timeout_ms; waited += 5U) {
		if (ctimer_alarm_fired(c)) {
			return true;
		}

		k_msleep(5);
	}

	return ctimer_alarm_fired(c);
}

void ctimer_report_configuration(void)
{
	TC_PRINT("%u CTIMER instance(s) under test\n", (unsigned int)CTIMER_CASE_COUNT);

	CTIMER_FOREACH_CASE(c) {
		TC_PRINT("  %s: CTIMER%u @ %p, %u Hz, gate %s\n", c->dev->name,
			 (unsigned int)c->index, (void *)c->base, counter_get_frequency(c->dev),
			 ctimer_gate_observable(c) ? "observable" : "NOT observable on this SoC");
	}
}

void ctimer_report_hw(const struct ctimer_case *c, const char *when)
{
	if (!ctimer_gate_observable(c)) {
		TC_PRINT("hw(%s, %s) gate not observable on this SoC\n", when, c->dev->name);
		return;
	}

	if (!ctimer_gate_is_open(c)) {
		/* No register read: an unclocked block does not answer. */
		TC_PRINT("hw(%s, %s) gate=0, registers not read\n", when, c->dev->name);
		return;
	}

	struct ctimer_image img;

	ctimer_image_read(c, &img);
	TC_PRINT("hw(%s, %s) gate=1 TCR=0x%08x MCR=0x%08x MR%u=0x%08x\n", when, c->dev->name,
		 img.tcr, img.mcr, (unsigned int)CTIMER_ALARM_CHANNEL, img.mr);
}
