/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include "drivers/GPIO/gpio_shiftReg.h"
}

/*
 * Three chained 74HC595, as on the CS7000 and CS7000P. The SPI transfers are
 * recorded to check what would be shifted out to the registers.
 */
static constexpr size_t NUM_OUTPUTS = 24;
static constexpr size_t NUM_BYTES = (NUM_OUTPUTS + 7) / 8;

static uint8_t lastTx[8];
static size_t lastTxSize = 0;
static unsigned spiTransfers = 0;
static unsigned strobePulses = 0;

static int fakeMode(const struct gpioDev *dev, const uint8_t pin,
                    const uint16_t mode)
{
    (void)dev;
    (void)pin;
    (void)mode;

    return 0;
}

static void fakeSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;

    strobePulses++;
}

static void fakeClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
}

static bool fakeRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;

    return false;
}

static const struct gpioApi fakeApi = { fakeMode, fakeSet, fakeClear,
                                        fakeRead };
static const struct gpioDev fakePort = { &fakeApi, NULL };

static int fakeTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    (void)dev;
    (void)rxBuf;

    if (size > sizeof(lastTx))
        return -EINVAL;

    memcpy(lastTx, txBuf, size);
    lastTxSize = size;
    spiTransfers++;

    return 0;
}

static const struct spiDevice fakeSpi = { fakeTransfer, NULL, NULL };
static const struct gpioPin strobe = { &fakePort, 8 };

/*
 * Same layout as GPIO_SHIFTREG_DEVICE_DEFINE(), spelled out because the macro
 * relies on C99 designated initializers.
 */
extern "C" {
extern const struct gpioApi gpioShiftReg_ops;
}

static uint8_t srData_extGpio[NUM_BYTES];
static const struct gpioShiftRegPriv srPriv = { &fakeSpi, strobe, NUM_OUTPUTS,
                                                srData_extGpio };
static const struct gpioDev extGpio = { &gpioShiftReg_ops, &srPriv };

static void resetDevice()
{
    memset(srData_extGpio, 0xFF, sizeof(srData_extGpio));
    memset(lastTx, 0x00, sizeof(lastTx));
    lastTxSize = 0;
    spiTransfers = 0;
    strobePulses = 0;

    gpioShiftReg_init(&extGpio);
}

static bool outputsAre(uint8_t b0, uint8_t b1, uint8_t b2)
{
    return (srData_extGpio[0] == b0) && (srData_extGpio[1] == b1)
        && (srData_extGpio[2] == b2);
}

TEST_CASE("Shift register init clears all the outputs", "[gpio][shiftreg]")
{
    resetDevice();

    REQUIRE(spiTransfers == 1);
    REQUIRE(strobePulses == 1);
    REQUIRE(lastTxSize == NUM_BYTES);
    REQUIRE(outputsAre(0x00, 0x00, 0x00));
    REQUIRE(memcmp(lastTx, srData_extGpio, NUM_BYTES) == 0);
}

TEST_CASE("Shift register set, clear and read address the right bits",
          "[gpio][shiftreg]")
{
    resetDevice();

    // Output 0 is the LSB of the last byte shifted out, output 23 the MSB of
    // the first one.
    gpioDev_set(&extGpio, 0);
    REQUIRE(outputsAre(0x00, 0x00, 0x01));
    REQUIRE(gpioDev_read(&extGpio, 0) == true);

    gpioDev_set(&extGpio, NUM_OUTPUTS - 1);
    REQUIRE(outputsAre(0x80, 0x00, 0x01));
    REQUIRE(gpioDev_read(&extGpio, NUM_OUTPUTS - 1) == true);

    gpioDev_set(&extGpio, 8);
    REQUIRE(outputsAre(0x80, 0x01, 0x01));

    REQUIRE(spiTransfers == 4);
    REQUIRE(lastTxSize == NUM_BYTES);
    REQUIRE(memcmp(lastTx, srData_extGpio, NUM_BYTES) == 0);

    gpioDev_clear(&extGpio, NUM_OUTPUTS - 1);
    REQUIRE(outputsAre(0x00, 0x01, 0x01));
    REQUIRE(gpioDev_read(&extGpio, NUM_OUTPUTS - 1) == false);
    REQUIRE(gpioDev_read(&extGpio, 8) == true);
    REQUIRE(spiTransfers == 5);
}

TEST_CASE("Shift register rejects pin numbers outside the output range",
          "[gpio][shiftreg]")
{
    resetDevice();
    gpioDev_set(&extGpio, 0);

    const unsigned transfers = spiTransfers;

    // One past the last output: byte index would wrap around to
    // (size_t)-1 / 8 and corrupt memory far away from the state buffer.
    gpioDev_set(&extGpio, NUM_OUTPUTS);
    gpioDev_clear(&extGpio, NUM_OUTPUTS);
    REQUIRE(gpioDev_read(&extGpio, NUM_OUTPUTS) == false);

    gpioDev_set(&extGpio, 255);
    gpioDev_clear(&extGpio, 255);
    REQUIRE(gpioDev_read(&extGpio, 255) == false);

    // Nothing shifted out, state untouched
    REQUIRE(spiTransfers == transfers);
    REQUIRE(outputsAre(0x00, 0x00, 0x01));

    // Outputs have a fixed direction
    REQUIRE(gpioDev_setMode(&extGpio, 0, OUTPUT) == -ENOTSUP);
}
