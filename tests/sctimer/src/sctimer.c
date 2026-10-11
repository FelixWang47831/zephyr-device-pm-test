/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The SCTimer wiring, and the hardware truths this case asserts on.
 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/tc_util.h>

#include <zephyr/dt-bindings/clock/mcux_lpc_syscon_clock.h>
#include <fsl_clock.h>
#include <soc.h>

#include "sctimer.h"

/*
 * The clock gate is read out of the MCX clock controller's own register, so a part
 * wiring this driver to some other clock controller would need a different mapping
 * and is refused here rather than read out of the wrong register.
 */
#define SCTIMER_ASSERT_SYSCON_CLOCK(node_id)                                                       \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CLOCKS_CTLR(node_id), nxp_lpc_syscon),                  \
		     "this case reads the SCTimer clock gate out of the MCX clock controller "      \
		     "and only fits SoCs whose SCTimer clock comes from nxp,lpc-syscon");

DT_FOREACH_STATUS_OKAY(nxp_sctimer_pwm, SCTIMER_ASSERT_SYSCON_CLOCK)

#define SCTIMER_ENTRY(node_id)                                                                     \
	{                                                                                          \
		.dev = DEVICE_DT_GET(node_id),                                                     \
		.base = (uintptr_t)DT_REG_ADDR(node_id),                                           \
	},

const struct sctimer_case sctimer_cases[SCTIMER_CASE_COUNT] = {
	DT_FOREACH_STATUS_OKAY(nxp_sctimer_pwm, SCTIMER_ENTRY)
};

/*
 * SCT_CLOCKS carries the gate constant for whichever SoC is being built -- the enum
 * is named kCLOCK_Sct on some parts and kCLOCK_Sct0 on others -- and is absent on
 * parts with no SCTimer. Same table the syscon clock driver's own SCTimer cases use.
 */
#if defined(SCT_CLOCKS)
static const clock_ip_name_t sct_clocks[] = SCT_CLOCKS;
#endif

/* The match register pair the SDK allocates for the first channel it sets up. */
#define SCTIMER_MATCH_PERIOD 0U
#define SCTIMER_MATCH_PULSE  1U

/*
 * The last match reload register, and a value that cannot be mistaken for a reset.
 * The SDK hands out match registers from 0 upwards, one pair per channel, and this
 * board target routes one output, so the top of the array is never written by the
 * driver. 24 bits, because the register is 16-bit wide when the counter is split in
 * two and this must read back unchanged in either configuration.
 */
#define SCTIMER_MATCH_WITNESS (FSL_FEATURE_SCT_NUMBER_OF_MATCH_CAPTURE - 1U)
#define SCTIMER_WITNESS_VALUE 0x00A5A5U

static SCT_Type *sctimer_base(const struct sctimer_case *c)
{
	return (SCT_Type *)c->base;
}

bool sctimer_gate_observable(const struct sctimer_case *c)
{
	ARG_UNUSED(c);

#if defined(SCT_CLOCKS)
	return true;
#else
	return false;
#endif
}

/*
 * CLOCK_EnableClock()/CLOCK_DisableClock() write through the write-only SET/CLR
 * aliases, so the readable register is the one at the head of the group:
 * SYSCON->AHBCLKCTRL0..3, contiguous from 0x200.
 */
bool sctimer_gate_is_open(const struct sctimer_case *c)
{
	ARG_UNUSED(c);

#if defined(SCT_CLOCKS)
	const volatile uint32_t *reg =
		&(&SYSCON->AHBCLKCTRL0)[CLK_GATE_ABSTRACT_REG_OFFSET(sct_clocks[0])];

	return (*reg & BIT(CLK_GATE_ABSTRACT_BITS_SHIFT(sct_clocks[0]))) != 0U;
#else
	return false;
#endif
}

void sctimer_gate_open(const struct sctimer_case *c)
{
	ARG_UNUSED(c);

#if defined(SCT_CLOCKS)
	CLOCK_EnableClock(sct_clocks[0]);
#endif
}

void sctimer_gate_close(const struct sctimer_case *c)
{
	ARG_UNUSED(c);

#if defined(SCT_CLOCKS)
	CLOCK_DisableClock(sct_clocks[0]);
#endif
}

void sctimer_image_read(const struct sctimer_case *c, struct sctimer_image *img)
{
	SCT_Type *base = sctimer_base(c);

	img->matchrel_period = base->MATCHREL[SCTIMER_MATCH_PERIOD];
	img->matchrel_pulse = base->MATCHREL[SCTIMER_MATCH_PULSE];
	img->out_set = base->OUT[SCTIMER_PWM_CHANNEL].SET;
	img->out_clr = base->OUT[SCTIMER_PWM_CHANNEL].CLR;
}

bool sctimer_image_equal(const struct sctimer_image *a, const struct sctimer_image *b)
{
	return (a->matchrel_period == b->matchrel_period) &&
	       (a->matchrel_pulse == b->matchrel_pulse) && (a->out_set == b->out_set) &&
	       (a->out_clr == b->out_clr);
}

bool sctimer_image_is_clear(const struct sctimer_image *img)
{
	return (img->matchrel_period == 0U) && (img->matchrel_pulse == 0U) &&
	       (img->out_set == 0U) && (img->out_clr == 0U);
}

void sctimer_clobber(const struct sctimer_case *c, struct sctimer_image *clobbered)
{
	SCT_Type *base = sctimer_base(c);
	bool was_open = sctimer_gate_is_open(c);

	/* The registers cannot be written while the block is unclocked, and this is
	 * called from a layer where the driver has already gated it.
	 */
	sctimer_gate_open(c);

	base->MATCHREL[SCTIMER_MATCH_PERIOD] = 0U;
	base->MATCHREL[SCTIMER_MATCH_PULSE] = 0U;
	base->OUT[SCTIMER_PWM_CHANNEL].SET = 0U;
	base->OUT[SCTIMER_PWM_CHANNEL].CLR = 0U;

	sctimer_image_read(c, clobbered);

	if (!was_open) {
		sctimer_gate_close(c);
	}
}

void sctimer_peek_gated(const struct sctimer_case *c, struct sctimer_peek *p)
{
	bool was_open = sctimer_gate_is_open(c);

	sctimer_gate_open(c);

	sctimer_image_read(c, &p->image);
	p->running = sctimer_running(c);
	p->output_high = sctimer_output_is_high(c);

	if (!was_open) {
		sctimer_gate_close(c);
	}
}

void sctimer_witness_write(const struct sctimer_case *c)
{
	sctimer_base(c)->MATCHREL[SCTIMER_MATCH_WITNESS] = SCTIMER_WITNESS_VALUE;
}

bool sctimer_witness_survives(const struct sctimer_case *c)
{
	return sctimer_base(c)->MATCHREL[SCTIMER_MATCH_WITNESS] == SCTIMER_WITNESS_VALUE;
}

bool sctimer_running(const struct sctimer_case *c)
{
	return (sctimer_base(c)->CTRL & SCT_CTRL_HALT_L_MASK) == 0U;
}


bool sctimer_output_is_high(const struct sctimer_case *c)
{
	return (sctimer_base(c)->OUTPUT & BIT(SCTIMER_PWM_CHANNEL)) != 0U;
}

bool sctimer_advances(const struct sctimer_case *c)
{
	SCT_Type *base = sctimer_base(c);
	uint32_t first = base->COUNT;

	/*
	 * Busy-waited rather than slept: the counter is clocked from the bus clock,
	 * so this is thousands of ticks even at the slowest bus frequency any board
	 * here runs, and a sleep would let the idle thread enter a power state in the
	 * layers that have one.
	 */
	k_busy_wait(200);

	return base->COUNT != first;
}

/*
 * A level read once says nothing -- a stopped output sits at some level too. This
 * samples until it has seen the output both high and low, which only a running PWM
 * produces. Bounded by a cycle count rather than by time, so it fails rather than
 * hangs if the output is parked.
 */
bool sctimer_output_toggles(const struct sctimer_case *c)
{
	bool seen_high = false;
	bool seen_low = false;

	for (unsigned int i = 0; i < 4U * SCTIMER_PWM_PERIOD; i++) {
		if (sctimer_output_is_high(c)) {
			seen_high = true;
		} else {
			seen_low = true;
		}

		if (seen_high && seen_low) {
			return true;
		}
	}

	return false;
}

int sctimer_configure(const struct sctimer_case *c)
{
	return pwm_set_cycles(c->dev, SCTIMER_PWM_CHANNEL, SCTIMER_PWM_PERIOD,
			      SCTIMER_PWM_PULSE, 0);
}

int sctimer_configure_and_verify(const struct sctimer_case *c)
{
	int err = sctimer_configure(c);

	if (err != 0) {
		return err;
	}

	return sctimer_advances(c) ? 0 : -ESTALE;
}

void sctimer_report_configuration(void)
{
	TC_PRINT("%u SCTimer instance(s) under test\n", (unsigned int)SCTIMER_CASE_COUNT);

	SCTIMER_FOREACH_CASE(c) {
		uint64_t cycles = 0;

		(void)pwm_get_cycles_per_sec(c->dev, SCTIMER_PWM_CHANNEL, &cycles);
		TC_PRINT("  %s: @ %p, %llu Hz, channel %u, gate %s\n", c->dev->name,
			 (void *)c->base, cycles, (unsigned int)SCTIMER_PWM_CHANNEL,
			 sctimer_gate_observable(c) ? "observable"
						    : "NOT observable on this SoC");
	}
}

void sctimer_report_hw(const struct sctimer_case *c, const char *when)
{
	if (!sctimer_gate_observable(c)) {
		TC_PRINT("hw(%s, %s) gate not observable on this SoC\n", when, c->dev->name);
		return;
	}

	if (!sctimer_gate_is_open(c)) {
		/* No register read: an unclocked block does not answer. */
		TC_PRINT("hw(%s, %s) gate=0, registers not read\n", when, c->dev->name);
		return;
	}

	struct sctimer_image img;

	sctimer_image_read(c, &img);
	TC_PRINT("hw(%s, %s) gate=1 halted=%u out=%u MATCHREL0=0x%08x MATCHREL1=0x%08x "
		 "SET=0x%08x CLR=0x%08x\n", when, c->dev->name, sctimer_running(c) ? 0U : 1U,
		 sctimer_output_is_high(c) ? 1U : 0U, img.matchrel_period, img.matchrel_pulse,
		 img.out_set, img.out_clr);
}
