/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The register view of the OPAMP block, shared by every layer.
 */

#include <zephyr/sys/util.h>
#include <zephyr/tc_util.h>

#include <fsl_device_registers.h>

#include "opamp.h"

const struct device *const opamp_dev = DEVICE_DT_GET(OPAMP_NODE);

static OPAMP_Type *const base = (OPAMP_Type *)DT_REG_ADDR(OPAMP_NODE);

#if defined(CONFIG_OPAMP_MCUX_OPAMP)

#define OPAMP_GAIN_MASK (OPAMP_OPAMP_CTR_NGAIN_MASK | OPAMP_OPAMP_CTR_PGAIN_MASK)
#define OPAMP_CONFIG_MASK (~(OPAMP_OPAMP_CTR_EN_MASK | OPAMP_GAIN_MASK))

bool opamp_en(void)
{
	return (base->OPAMP_CTR & OPAMP_OPAMP_CTR_EN_MASK) != 0U;
}

uint32_t opamp_config_bits(void)
{
	return base->OPAMP_CTR & OPAMP_CONFIG_MASK;
}

uint32_t opamp_gain_fields(void)
{
	return base->OPAMP_CTR & OPAMP_GAIN_MASK;
}

void opamp_clobber(void)
{
	base->OPAMP_CTR = 0U;
}

int opamp_configure_gain(enum opamp_gain gain)
{
	return opamp_set_gain(OPAMP_DEV, gain);
}

void opamp_report_hw(const char *when)
{
	TC_PRINT("hw(%s) OPAMP_CTR=0x%08x EN=%u gain=0x%08x\n", when, base->OPAMP_CTR,
		 opamp_en() ? 1U : 0U, opamp_gain_fields());
}

#elif defined(CONFIG_OPAMP_MCUX_OPAMP_FAST)

#define OPAMP_CONFIG_MASK (~OPAMP_OPAMP_CTRL_OPA_EN_MASK)

bool opamp_en(void)
{
	return (base->OPAMP_CTRL & OPAMP_OPAMP_CTRL_OPA_EN_MASK) != 0U;
}

uint32_t opamp_config_bits(void)
{
	return base->OPAMP_CTRL & OPAMP_CONFIG_MASK;
}

void opamp_clobber(void)
{
	base->OPAMP_CTRL = 0U;
}

void opamp_report_hw(const char *when)
{
	TC_PRINT("hw(%s) OPAMP_CTRL=0x%08x EN=%u\n", when, base->OPAMP_CTRL,
		 opamp_en() ? 1U : 0U);
}

#endif
