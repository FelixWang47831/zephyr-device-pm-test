/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief What every layer of the OPAMP case needs from the hardware.
 *
 * The opamp API has no enable or disable call: being PM_DEVICE_STATE_ACTIVE is the
 * only thing that turns the amplifier on, so the EN bit this case reads tracks the
 * PM state 1:1, with no "configured but not driving" phase like LPDAC's DACEN. What
 * this case observes is the register block directly, not the driver's view of it,
 * the same way tests/lpcmp does: a PM state machine that has drifted away from the
 * hardware is exactly what reading the register catches.
 *
 * Two driver variants share this header. CONFIG_OPAMP_MCUX_OPAMP (register
 * OPAMP_CTR) implements set_gain() and is the only one the gain assertions run
 * against; CONFIG_OPAMP_MCUX_OPAMP_FAST (register OPAMP_CTRL) returns -ENOTSUP for
 * set_gain() unconditionally and has no gain fields to read back.
 */

#ifndef OPAMP_PM_H_
#define OPAMP_PM_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/opamp.h>

#if !DT_NODE_EXISTS(DT_ALIAS(test_opamp))
#error "This board has no OPAMP wiring: add boards/<board_target>.overlay with a test-opamp alias"
#endif

#define OPAMP_NODE DT_ALIAS(test_opamp)

#if !defined(CONFIG_OPAMP_MCUX_OPAMP) && !defined(CONFIG_OPAMP_MCUX_OPAMP_FAST)
#error "Neither CONFIG_OPAMP_MCUX_OPAMP nor CONFIG_OPAMP_MCUX_OPAMP_FAST is enabled"
#endif

/** The opamp under test. */
extern const struct device *const opamp_dev;
#define OPAMP_DEV opamp_dev

/** True while the hardware EN bit says the amplifier is driving its output. */
bool opamp_en(void);

/**
 * @brief The devicetree configuration half of the register block.
 *
 * Everything PM_DEVICE_ACTION_TURN_ON rebuilds: the fixed fields set_gain() does
 * not touch and SUSPEND/RESUME do not touch either. Excludes EN, and on the
 * gain-capable variant also excludes the gain fields, which TURN_ON leaves at
 * their reset value -- RESUME is what replays them.
 */
uint32_t opamp_config_bits(void);

/**
 * @brief Clear the register block, leaving it looking the way a reset leaves it.
 *
 * What stands in for a power cycle in a layer where nothing really cut power to
 * the block, so that TURN_ON has something to restore.
 */
void opamp_clobber(void);

/**
 * @brief Print the register view.
 *
 * @param when short label for where in the sequence this is, e.g. "before".
 */
void opamp_report_hw(const char *when);

#if defined(CONFIG_OPAMP_MCUX_OPAMP)
/**
 * @brief Apply a gain through the driver's public API.
 *
 * A thin wrapper so every layer asks for gain the same way a real consumer would,
 * through opamp_set_gain(), rather than reaching into the driver.
 *
 * @return 0 on success, negative errno from opamp_set_gain() otherwise.
 */
int opamp_configure_gain(enum opamp_gain gain);

/**
 * @brief NGAIN and PGAIN together: the one pair of fields set_gain() writes and
 *        TURN_ON does not rebuild -- RESUME replays them from driver state instead.
 */
uint32_t opamp_gain_fields(void);
#endif /* CONFIG_OPAMP_MCUX_OPAMP */

#endif /* OPAMP_PM_H_ */
