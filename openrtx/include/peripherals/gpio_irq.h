/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPIO_IRQ_H
#define GPIO_IRQ_H

#include "peripherals/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * External interrupt on a gpio pin.
 *
 * A pin is attached to an interrupt line together with a callback that runs
 * in interrupt context whenever the selected edge is seen on the pin. The
 * callback must only latch flags, count, timestamp or wake up a thread: it
 * must never block, allocate or talk to a bus (the SPI drivers take mutexes).
 *
 * The number of lines and their sharing rules depend on the MCU. On the STM32
 * the line number is the pin number (0-15) and the port is selected in the
 * SYSCFG multiplexer, so two pins with the same number on different ports
 * cannot be attached at the same time.
 */

/**
 * Edge(s) firing the interrupt.
 */
enum gpioIrqEdge {
    GPIO_IRQ_FALLING = 0, /**< High-to-low transition */
    GPIO_IRQ_RISING = 1,  /**< Low-to-high transition */
    GPIO_IRQ_BOTH = 2,    /**< Either transition */
};

/**
 * Attach an interrupt callback to a gpio pin and enable the interrupt.
 * The pin must have been configured as an input beforehand.
 *
 * @param pin: gpio pin descriptor.
 * @param edge: edge(s) firing the interrupt.
 * @param cb: callback executed in interrupt context on every edge.
 * @param arg: opaque argument passed to the callback.
 * @return zero on success, -1 if the interrupt line of the pin is already in
 * use, if the pin cannot generate interrupts or if the callback is NULL.
 */
int gpioIrq_attach(const struct gpioPin *pin, enum gpioIrqEdge edge,
                   void (*cb)(void *arg), void *arg);

/**
 * Disable the interrupt of a gpio pin and remove its callback.
 * Calling this function on a pin that was never attached does nothing.
 *
 * @param pin: gpio pin descriptor.
 */
void gpioIrq_detach(const struct gpioPin *pin);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_IRQ_H */
