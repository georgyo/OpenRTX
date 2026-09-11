/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "drivers/baseband/AK2365A.h"
#include "interfaces/delays.h"

/*
 * Recording mocks for the gpio, SPI and delay primitives used by the AK2365A
 * driver. Every action is appended to a single event list, so that both the
 * register values and the ordering of writes and delays can be checked.
 */
enum class EvType { REG_WRITE, DELAY, RES_LOW, RES_HIGH };

struct Event {
    EvType type;
    uint8_t reg;
    uint8_t value;
    uint32_t us;
};

static constexpr uint8_t CS_PIN = 1;
static constexpr uint8_t RES_PIN = 2;

static std::vector<Event> events;

static int mockMode(const struct gpioDev *dev, const uint8_t pin,
                    const uint16_t mode)
{
    (void)dev;
    (void)pin;
    (void)mode;
    return 0;
}

static void mockSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    if (pin == RES_PIN)
        events.push_back({ EvType::RES_HIGH, 0, 0, 0 });
}

static void mockClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    if (pin == RES_PIN)
        events.push_back({ EvType::RES_LOW, 0, 0, 0 });
}

static bool mockRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
    return false;
}

static int mockTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    (void)dev;
    (void)rxBuf;

    REQUIRE(size == 2);
    const uint8_t *data = static_cast<const uint8_t *>(txBuf);
    REQUIRE((data[0] & 0x01) == 0); // Write access, register in bits 6:1
    events.push_back(
        { EvType::REG_WRITE, static_cast<uint8_t>(data[0] >> 1), data[1], 0 });
    return 0;
}

extern "C" void delayUs(unsigned int useconds)
{
    events.push_back({ EvType::DELAY, 0, 0, useconds });
}

extern "C" void delayMs(unsigned int mseconds)
{
    events.push_back({ EvType::DELAY, 0, 0, mseconds * 1000 });
}

static const struct gpioApi mockGpioApi = { mockMode, mockSet, mockClear,
                                            mockRead };
static const struct gpioDev mockGpio = { &mockGpioApi, nullptr };
static const struct spiDevice mockSpi = { mockTransfer, nullptr, nullptr };
static const struct ak2365a detector = { &mockSpi,
                                         { &mockGpio, CS_PIN },
                                         { &mockGpio, RES_PIN } };

/*
 * Return the index of the first write of a given register, or -1.
 */
static int findWrite(const uint8_t reg, const size_t from = 0)
{
    for (size_t i = from; i < events.size(); i++) {
        if ((events[i].type == EvType::REG_WRITE) && (events[i].reg == reg))
            return static_cast<int>(i);
    }

    return -1;
}

/*
 * Check that every write of a given register carries the expected value and
 * that the register is written at least once.
 */
static void requireAllWrites(const uint8_t reg, const uint8_t value)
{
    size_t count = 0;
    for (const auto &ev : events) {
        if ((ev.type == EvType::REG_WRITE) && (ev.reg == reg)) {
            REQUIRE(ev.value == value);
            count += 1;
        }
    }

    REQUIRE(count > 0);
}

/*
 * Sum of the delays issued between two event indexes (end excluded).
 */
static uint32_t delayBetween(const size_t begin, const size_t end)
{
    uint32_t total = 0;
    for (size_t i = begin; i < end; i++) {
        if (events[i].type == EvType::DELAY)
            total += events[i].us;
    }

    return total;
}

TEST_CASE("AK2365A 25kHz configuration selects F0 filter and wide level",
          "[ak2365a]")
{
    events.clear();
    AK2365A_setFilterBandwidth(&detector, AK2365A_BPF_7p5, AK2365A_BAND_WIDE);

    // BS = 111, BAND = 1, BPF_BW[1:0] = 00, LOFREQ = 01
    requireAllWrites(0x01, 0xF1);
    // BPF_BW[2] = 1, AGC auto, AGC1 gain code 1
    requireAllWrites(0x0B, 0x81);
}

TEST_CASE("AK2365A 12.5kHz configuration selects F1 filter and narrow level",
          "[ak2365a]")
{
    events.clear();
    AK2365A_setFilterBandwidth(&detector, AK2365A_BPF_6, AK2365A_BAND_NARROW);

    // BS = 111, BAND = 0, BPF_BW[1:0] = 00, LOFREQ = 01
    requireAllWrites(0x01, 0xE1);
    // BPF_BW[2] = 0
    requireAllWrites(0x0B, 0x01);
}

TEST_CASE("AK2365A narrower filters are selected through BPF_BW[1:0]",
          "[ak2365a]")
{
    events.clear();
    AK2365A_setFilterBandwidth(&detector, AK2365A_BPF_4p5, AK2365A_BAND_NARROW);
    requireAllWrites(0x01, 0xE5);
    requireAllWrites(0x0B, 0x01);

    events.clear();
    AK2365A_setFilterBandwidth(&detector, AK2365A_BPF_3, AK2365A_BAND_NARROW);
    requireAllWrites(0x01, 0xE9);
    requireAllWrites(0x0B, 0x01);

    events.clear();
    AK2365A_setFilterBandwidth(&detector, AK2365A_BPF_2, AK2365A_BAND_NARROW);
    requireAllWrites(0x01, 0xED);
    requireAllWrites(0x0B, 0x01);

    // BAND is independent from the filter selection
    events.clear();
    AK2365A_setFilterBandwidth(&detector, AK2365A_BPF_4p5, AK2365A_BAND_WIDE);
    requireAllWrites(0x01, 0xF5);
    requireAllWrites(0x0B, 0x01);
}

TEST_CASE("AK2365A setFilterBandwidth keeps the receiver in operating mode 7",
          "[ak2365a]")
{
    events.clear();
    AK2365A_setFilterBandwidth(&detector, AK2365A_BPF_7p5, AK2365A_BAND_WIDE);

    for (const auto &ev : events) {
        if ((ev.type == EvType::REG_WRITE) && (ev.reg == 0x01))
            REQUIRE((ev.value & 0xE0) == 0xE0);

        // No calibration trigger outside of init
        if ((ev.type == EvType::REG_WRITE) && (ev.reg == 0x02))
            REQUIRE((ev.value & 0x01) == 0);
    }
}

TEST_CASE("AK2365A init follows the datasheet calibration procedure",
          "[ak2365a]")
{
    events.clear();
    AK2365A_init(&detector);

    // Hardware reset pulse of at least 1us, before any register access
    int resLow = -1;
    int resHigh = -1;
    for (size_t i = 0; i < events.size(); i++) {
        if ((events[i].type == EvType::RES_LOW) && (resLow < 0))
            resLow = static_cast<int>(i);
        if ((events[i].type == EvType::RES_HIGH) && (resHigh < 0))
            resHigh = static_cast<int>(i);
    }

    REQUIRE(resLow >= 0);
    REQUIRE(resHigh > resLow);
    REQUIRE(delayBetween(resLow, resHigh) >= 1);
    REQUIRE(findWrite(0x04) > resHigh);
    REQUIRE(events[findWrite(0x04)].value == 0xAA); // Software reset

    // Operating mode 6 for at least 500us before calibration is triggered
    int mode6 = findWrite(0x01);
    REQUIRE(mode6 > findWrite(0x04));
    REQUIRE(events[mode6].value == 0xC1);

    int cal = findWrite(0x02);
    REQUIRE(cal > mode6);
    REQUIRE((events[cal].value & 0x01) == 0x01);
    REQUIRE(delayBetween(mode6, cal) >= 500);

    // Only one calibration trigger and no mode change while calibrating
    REQUIRE(findWrite(0x02, cal + 1) < 0);
    REQUIRE(findWrite(0x01, mode6 + 1) < 0);

    // 1.3ms of calibration plus 1.5ms of discriminator settling
    REQUIRE(delayBetween(cal, events.size()) >= 2800);
}
