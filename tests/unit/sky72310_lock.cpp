/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/baseband/SKY72310.h"
#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>

/*
 * Fake gpio port: the lock detect pin reads low for a programmable number of
 * reads, then high. A negative count keeps it low forever.
 */
static constexpr uint8_t CS_PIN = 1;
static constexpr uint8_t LD_PIN = 2;

static int readsBeforeLock = 0;
static int ldReads = 0;
static uint16_t pinMode[16] = { 0 };
static int spiWrites = 0;

static int fakeMode(const struct gpioDev *dev, const uint8_t pin,
                    const uint16_t mode)
{
    (void)dev;
    pinMode[pin] = mode;
    return 0;
}

static void fakeSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
}

static void fakeClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
}

static bool fakeRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    REQUIRE(pin == LD_PIN);

    ldReads += 1;
    if (readsBeforeLock < 0)
        return false;

    return (ldReads > readsBeforeLock);
}

static int fakeSpiTransfer(const struct spiDevice *dev, const void *txBuf,
                           void *rxBuf, const size_t size)
{
    (void)dev;
    (void)txBuf;
    (void)rxBuf;
    (void)size;
    spiWrites += 1;
    return 0;
}

static const struct gpioApi fakeApi = { fakeMode, fakeSet, fakeClear,
                                        fakeRead };
static const struct gpioDev fakePort = { &fakeApi, nullptr };
static const struct spiDevice fakeSpi = { fakeSpiTransfer, nullptr, nullptr };

static const struct sky73210 pllWithLd = { &fakeSpi,
                                           { &fakePort, CS_PIN },
                                           { &fakePort, LD_PIN },
                                           16800000 };

static const struct sky73210 pllNoLd = { &fakeSpi,
                                         { &fakePort, CS_PIN },
                                         { nullptr, 0 },
                                         16800000 };

static void reset(int lockAfter)
{
    readsBeforeLock = lockAfter;
    ldReads = 0;
    spiWrites = 0;
    for (auto &m : pinMode)
        m = 0xFF;
}

TEST_CASE("SKY72310 init configures the lock detect pin as input",
          "[sky72310][pll]")
{
    reset(0);
    SKY73210_init(&pllWithLd, 16);

    REQUIRE(pinMode[CS_PIN] == OUTPUT);
    REQUIRE(pinMode[LD_PIN] == INPUT);
    REQUIRE(spiWrites == 4);
}

TEST_CASE("SKY72310 init without lock detect pin touches only chip select",
          "[sky72310][pll]")
{
    reset(0);
    SKY73210_init(&pllNoLd, 16);

    REQUIRE(pinMode[CS_PIN] == OUTPUT);
    REQUIRE(pinMode[LD_PIN] == 0xFF);
    REQUIRE(spiWrites == 4);
}

TEST_CASE("SKY72310 lock wait returns as soon as LD goes high",
          "[sky72310][pll]")
{
    reset(3);

    REQUIRE(SKY73210_waitLock(&pllWithLd, 5000) == true);
    REQUIRE(ldReads == 4);
}

TEST_CASE("SKY72310 lock wait returns immediately when already locked",
          "[sky72310][pll]")
{
    reset(0);

    REQUIRE(SKY73210_isLocked(&pllWithLd) == true);
    REQUIRE(ldReads == 1);

    reset(0);
    REQUIRE(SKY73210_waitLock(&pllWithLd, 5000) == true);
    REQUIRE(ldReads == 1);
}

TEST_CASE("SKY72310 lock wait times out while LD stays low", "[sky72310][pll]")
{
    reset(-1);

    REQUIRE(SKY73210_isLocked(&pllWithLd) == false);

    reset(-1);
    auto start = std::chrono::steady_clock::now();
    REQUIRE(SKY73210_waitLock(&pllWithLd, 5000) == false);
    auto elapsed = std::chrono::steady_clock::now() - start;

    // One read before each 100us poll interval, plus the final one
    REQUIRE(ldReads == 51);
    REQUIRE(elapsed >= std::chrono::microseconds(5000));
}

TEST_CASE("SKY72310 lock wait with zero timeout samples LD once",
          "[sky72310][pll]")
{
    reset(-1);
    REQUIRE(SKY73210_waitLock(&pllWithLd, 0) == false);
    REQUIRE(ldReads == 1);

    reset(0);
    REQUIRE(SKY73210_waitLock(&pllWithLd, 0) == true);
    REQUIRE(ldReads == 1);
}

TEST_CASE("SKY72310 without lock detect pin always reports locked",
          "[sky72310][pll]")
{
    reset(-1);

    REQUIRE(SKY73210_isLocked(&pllNoLd) == true);
    REQUIRE(SKY73210_waitLock(&pllNoLd, 5000) == true);
    REQUIRE(ldReads == 0);
}
