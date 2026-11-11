/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests of the CS7000 / CS7000-PLUS RF driver, radio_CS7000.cpp,
 * around its DMR branches: the real driver and the real HR_C6000 baseband
 * driver (HR_Cx000.cpp, HR_C6000.cpp, HR_C6000_CS7000.cpp) are compiled
 * against a recording fake "user" SPI bus, a fake gpio expander, fake
 * SKY72310 / AK2365A / DMR modem drivers and fake delays; the fake
 * hwconfig.h in tests/unit/fake/cs7000_radio shadows the target one.
 *
 * Every action of the driver lands in one event list in the order it
 * happened, so that the tests can compare the FM and DMR keying sequences
 * and check that the PA is never keyed before the PLL lock.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "calibration/calibInfo_CS7000.h"
#include "drivers/SPI/spi_bitbang.h"
#include "drivers/audio/stm32_adc.h"
#include "drivers/baseband/AK2365A.h"
#include "drivers/baseband/HR_C6000.h"
#include "drivers/baseband/SKY72310.h"
#include "hwconfig.h"
#include "interfaces/delays.h"
#include "interfaces/dmr_baseband.h"
#include "interfaces/nvmem.h"
#include "interfaces/radio.h"

#ifndef FAKE_CS7000_RADIO_HWCONFIG
#error "The fake CS7000 radio hwconfig.h must shadow the real one"
#endif

/*
 * Event record.
 */
enum class Ev {
    GPIO_SET,   /* gpio expander bit set, pin      */
    GPIO_CLEAR, /* gpio expander bit cleared, pin  */
    PLL_FREQ,   /* SKY73210_setFrequency, freq     */
    PLL_LOCK,   /* SKY73210_waitLock, result       */
    REG_WRITE,  /* HR_C6000 CONFIG register, reg, value */
    DMRBB_IDLE, /* dmrbb_idle()                    */
    DMRBB_RX,   /* dmrbb_startRx()                 */
    DMRBB_CFG,  /* dmrbb_configure()               */
    DMRBB_TERM, /* dmrbb_terminate()               */
    DET_INIT,   /* AK2365A_init()                  */
};

struct Event {
    Ev type;
    uint32_t a;
    uint32_t b;

    bool operator==(const Event &o) const
    {
        return (type == o.type) && (a == o.a) && (b == o.b);
    }
};

static std::vector<Event> events;

static Event gpioSet(uint8_t pin)
{
    return { Ev::GPIO_SET, pin, 0 };
}

static Event gpioClear(uint8_t pin)
{
    return { Ev::GPIO_CLEAR, pin, 0 };
}

static Event regWrite(uint8_t reg, uint8_t value)
{
    return { Ev::REG_WRITE, reg, value };
}

/* Expander bits of the pinmap */
static constexpr uint8_t PIN_RF_APC_SW = 3;
static constexpr uint8_t PIN_VCOVCC_SW = 8;
static constexpr uint8_t PIN_TX_PWR_EN = 9;
static constexpr uint8_t PIN_RX_PWR_EN = 15;
static constexpr uint8_t PIN_VCO_PWR_EN = 11;

static constexpr uint8_t USPI_CS_PIN = 12;

/*
 * Fake registers and native gpio.
 */
FakeDacRegs fakeDac;
FakeRccRegs fakeRcc;
const int fakeGpioA = 0;

extern "C" void gpio_setMode(const void *port, const uint8_t pin,
                             const uint16_t mode)
{
    (void)port;
    (void)pin;
    (void)mode;
}

/*
 * Fake gpio expander: set/clear recorded by pin number.
 */
static int extMode(const struct gpioDev *dev, const uint8_t pin,
                   const uint16_t mode)
{
    (void)dev;
    (void)pin;
    (void)mode;
    return 0;
}

static void extSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    events.push_back(gpioSet(pin));
}

static void extClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    events.push_back(gpioClear(pin));
}

static bool extRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
    return false;
}

static const struct gpioApi extApi = { extMode, extSet, extClear, extRead };
const struct gpioDev extGpio = { &extApi, nullptr };

/*
 * Fake "user" SPI bus of the HR_C6000: the chip select on GpioB pin 12
 * delimits the transactions, a 3-byte write of the CONFIG page (opcode
 * 0x04) is recorded as a register write, everything else is counted.
 */
static std::vector<uint8_t> uspiCurrent;
static bool uspiCsLow = false;
static unsigned uspiOtherWrites = 0;

static void uspiEnd()
{
    if (uspiCurrent.size() == 3 && uspiCurrent[0] == 0x04)
        events.push_back(regWrite(uspiCurrent[1], uspiCurrent[2]));
    else if (!uspiCurrent.empty())
        uspiOtherWrites += 1;

    uspiCurrent.clear();
}

static int portBMode(const struct gpioDev *dev, const uint8_t pin,
                     const uint16_t mode)
{
    (void)dev;
    (void)pin;
    (void)mode;
    return 0;
}

static void portBSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    if (pin == USPI_CS_PIN) {
        uspiEnd();
        uspiCsLow = false;
    }
}

static void portBClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    if (pin == USPI_CS_PIN) {
        REQUIRE(uspiCsLow == false);
        uspiCsLow = true;
        uspiCurrent.clear();
    }
}

static bool portBRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
    return false;
}

static const struct gpioApi portBApi = { portBMode, portBSet, portBClear,
                                         portBRead };
const struct gpioDev GpioB = { &portBApi, nullptr };

static int uspiTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    (void)dev;
    REQUIRE(uspiCsLow == true);
    const uint8_t *tx = static_cast<const uint8_t *>(txBuf);
    for (size_t i = 0; i < size; i++)
        uspiCurrent.push_back((tx != nullptr) ? tx[i] : 0x00);

    if (rxBuf != nullptr)
        memset(rxBuf, 0, size);

    return 0;
}

static const struct spiDevice uspiDev = { uspiTransfer, nullptr, nullptr };

HR_C6000 C6000(&uspiDev, { &GpioB, USPI_CS_PIN });

/*
 * Fake bit-banged SPI devices of the PLL and the detector, never clocked
 * by the fakes below.
 */
static int nullTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    (void)dev;
    (void)txBuf;
    (void)rxBuf;
    (void)size;
    return 0;
}

const struct spiCustomDevice det_spi = { nullTransfer, nullptr, nullptr,
                                         nullptr };
const struct spiCustomDevice pll_spi = { nullTransfer, nullptr, nullptr,
                                         nullptr };

extern "C" int spiBitbang_init(const struct spiCustomDevice *dev)
{
    (void)dev;
    return 0;
}

extern "C" void spiBitbang_terminate(const struct spiCustomDevice *dev)
{
    (void)dev;
}

/*
 * Fake SKY72310 synthesizer: the lock result is set by the test.
 */
const struct sky73210 pll = { reinterpret_cast<const struct spiDevice *>(
                                  &pll_spi),
                              { &extGpio, 0 },
                              { &extGpio, 0 },
                              16800000 };
static bool pllLocks = true;

extern "C" void SKY73210_init(const struct sky73210 *dev, const uint8_t gain)
{
    (void)dev;
    (void)gain;
}

extern "C" void SKY73210_terminate(const struct sky73210 *dev)
{
    (void)dev;
}

extern "C" void SKY73210_setFrequency(const struct sky73210 *dev,
                                      const uint32_t freq, uint8_t clkDiv)
{
    (void)dev;
    (void)clkDiv;
    events.push_back({ Ev::PLL_FREQ, freq, 0 });
}

extern "C" bool SKY73210_isLocked(const struct sky73210 *dev)
{
    (void)dev;
    return pllLocks;
}

extern "C" bool SKY73210_waitLock(const struct sky73210 *dev,
                                  uint32_t timeoutUs)
{
    (void)dev;
    (void)timeoutUs;
    events.push_back({ Ev::PLL_LOCK, pllLocks ? 1u : 0u, 0 });
    return pllLocks;
}

/*
 * Fake AK2365A detector.
 */
const struct ak2365a detector = { reinterpret_cast<const struct spiDevice *>(
                                      &det_spi),
                                  { &extGpio, 0 },
                                  { &extGpio, 0 } };

extern "C" void AK2365A_init(const struct ak2365a *dev)
{
    (void)dev;
    events.push_back({ Ev::DET_INIT, 0, 0 });
}

extern "C" void AK2365A_terminate(const struct ak2365a *dev)
{
    (void)dev;
}

extern "C" void AK2365A_setFilterBandwidth(const struct ak2365a *dev,
                                           const enum AK2365A_BPF bpf,
                                           const enum AK2365A_BAND band)
{
    (void)dev;
    (void)bpf;
    (void)band;
}

/*
 * Fake DMR modem driver: the RF driver only starts, idles, configures and
 * terminates it.
 */
static struct dmrbbConfig lastBbConfig;

extern "C" void dmrbb_configure(const struct dmrbbConfig *cfg)
{
    lastBbConfig = *cfg;
    events.push_back({ Ev::DMRBB_CFG, 0, 0 });
}

extern "C" void dmrbb_startRx(void)
{
    events.push_back({ Ev::DMRBB_RX, 0, 0 });
}

extern "C" void dmrbb_idle(void)
{
    events.push_back({ Ev::DMRBB_IDLE, 0, 0 });
}

extern "C" void dmrbb_terminate(void)
{
    events.push_back({ Ev::DMRBB_TERM, 0, 0 });
}

/*
 * Fake ADC, CTCSS audio stream and calibration data.
 */
static uint16_t adcSample(const struct Adc *adc, const uint32_t channel)
{
    (void)adc;
    (void)channel;
    return 0;
}

const struct Adc adc1 = { adcSample, nullptr, nullptr,
                          ADC_COUNTS_TO_UV(3300000, 16) };

static int adcStart(const uint8_t instance, const void *config,
                    struct streamCtx *ctx)
{
    (void)instance;
    (void)config;
    ctx->running = 1;
    return 0;
}

static int adcData(struct streamCtx *ctx, stream_sample_t **buf)
{
    *buf = ctx->buffer;
    return 0;
}

static int adcSync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)ctx;
    (void)dirty;
    return 0;
}

static void adcStop(struct streamCtx *ctx)
{
    ctx->running = 0;
}

extern "C" void stm32adc_init(const uint8_t instance)
{
    (void)instance;
}

const struct audioDriver stm32_adc_audio_driver = { adcStart, adcData, adcSync,
                                                    adcStop, adcStop };

/*
 * Calibration: constant parameters per array, so that the frequency
 * interpolation returns the array's value whatever the frequency and the
 * test can tell which pair fed the modulation registers.
 */
static constexpr uint8_t CAL_DIG_I = 0x52;
static constexpr uint8_t CAL_DIG_Q = 0x51;
static constexpr uint8_t CAL_ANA_I = 0x62;
static constexpr uint8_t CAL_ANA_Q = 0x61;
static constexpr uint8_t CAL_PWR_HI = 0xC8;
static constexpr uint8_t CAL_PWR_MID = 0x64;

extern "C" void nvm_readCalibData(void *buf)
{
    struct CS7000Calib *cal = static_cast<struct CS7000Calib *>(buf);
    memset(cal, 0, sizeof(*cal));

    for (unsigned i = 0; i < 8; i++) {
        cal->txCalFreq[i] = 400000000u + (i * 10000000u);
        cal->rxCalFreq[i] = 400000000u + (i * 10000000u);
        cal->rxSensitivity[i] = 0x80;
        cal->txHighPwr[i] = CAL_PWR_HI;
        cal->txMiddlePwr[i] = CAL_PWR_MID;
        cal->txDigitalPathI[i] = CAL_DIG_I;
        cal->txDigitalPathQ[i] = CAL_DIG_Q;
        cal->txAnalogPathI[i] = CAL_ANA_I;
        cal->txAnalogPathQ[i] = CAL_ANA_Q;
        cal->errorRate[i] = 0x10;
    }
}

/*
 * Fake delays.
 */
extern "C" void delayUs(unsigned int useconds)
{
    (void)useconds;
}

extern "C" void delayMs(unsigned int mseconds)
{
    (void)mseconds;
}

extern "C" void sleepFor(unsigned int seconds, unsigned int mseconds)
{
    (void)seconds;
    (void)mseconds;
}

extern "C" long long getTick()
{
    return 0;
}

/*
 * Test helpers.
 */
static rtxStatus_t rtxCfg;

static void resetAll()
{
    events.clear();
    uspiCurrent.clear();
    uspiCsLow = false;
    uspiOtherWrites = 0;
    pllLocks = true;
    memset(&fakeDac, 0, sizeof(fakeDac));
    memset(&fakeRcc, 0, sizeof(fakeRcc));
    memset(&rtxCfg, 0, sizeof(rtxCfg));
    rtxCfg.rxFrequency = 433500000;
    rtxCfg.txFrequency = 433500000;
    rtxCfg.txPower = 5000;
    rtxCfg.bandwidth = BW_12_5;
    rtxCfg.dmr_srcId = 2345678;
    rtxCfg.dmr_txColorCode = 1;
    rtxCfg.dmr_timeslot = 2;
    rtxCfg.dmr_polite = 1;
}

/* Initialised driver in the given mode, RX stage as rtx.cpp leaves it */
static void bringUp(uint8_t opMode)
{
    resetAll();
    rtxCfg.opMode = opMode;
    radio_init(&rtxCfg);
    radio_setOpmode(static_cast<enum opmode>(opMode));
    radio_updateConfiguration();
    radio_enableRx();
    events.clear();
}

/* Events of the RF chain only: gpio expander and synthesizer */
static std::vector<Event> rfEvents()
{
    std::vector<Event> out;
    for (const auto &e : events) {
        if ((e.type == Ev::GPIO_SET) || (e.type == Ev::GPIO_CLEAR)
            || (e.type == Ev::PLL_FREQ) || (e.type == Ev::PLL_LOCK))
            out.push_back(e);
    }

    return out;
}

static size_t indexOf(const Event &e)
{
    for (size_t i = 0; i < events.size(); i++) {
        if (events[i] == e)
            return i;
    }

    FAIL("event not found");
    return 0;
}

static bool has(const Event &e)
{
    for (const auto &x : events) {
        if (x == e)
            return true;
    }

    return false;
}

/* Register 0x60 written with the analog TX start bit, or the mic bits */
static bool analogTxStarted()
{
    for (const auto &e : events) {
        if ((e.type == Ev::REG_WRITE) && (e.a == 0x60) && ((e.b & 0x80) != 0))
            return true;
    }

    return false;
}

/*
 * The RF keying sequence up to the mode-specific chip setup, common to
 * every mode: PA and LNA off, APC in power control mode, TX VCO, PLL on
 * the TX frequency, lock wait.
 */
static const std::vector<Event> keyingPrefix = {
    gpioClear(PIN_TX_PWR_EN),       gpioClear(PIN_RX_PWR_EN),
    gpioSet(PIN_RF_APC_SW),         gpioClear(PIN_VCOVCC_SW),
    { Ev::PLL_FREQ, 433500000, 0 }, { Ev::PLL_LOCK, 1, 0 },
};

TEST_CASE("FM radio_enableTx keys the RF chain and starts the analog TX",
          "[radio_cs7000]")
{
    bringUp(OPMODE_FM);
    radio_enableTx();

    REQUIRE(radio_getStatus() == TX);
    REQUIRE(analogTxStarted());

    std::vector<Event> rf = rfEvents();
    REQUIRE(rf.size() == keyingPrefix.size() + 1);
    for (size_t i = 0; i < keyingPrefix.size(); i++) {
        INFO("event " << i);
        REQUIRE(rf[i] == keyingPrefix[i]);
    }
    REQUIRE(rf.back() == gpioSet(PIN_TX_PWR_EN));

    /* The PA comes after the modulator setup, the lock before both */
    REQUIRE(indexOf({ Ev::PLL_LOCK, 1, 0 }) < indexOf(regWrite(0x60, 0x80)));
    REQUIRE(indexOf(regWrite(0x60, 0x80)) < indexOf(gpioSet(PIN_TX_PWR_EN)));

    /* APC from the high power calibration point at 5W */
    REQUIRE(fakeDac.DHR8R1 == CAL_PWR_HI);
}

TEST_CASE("DMR radio_enableTx keys the same RF chain and nothing else",
          "[radio_cs7000][dmr]")
{
    bringUp(OPMODE_DMR);
    radio_enableTx();

    REQUIRE(radio_getStatus() == TX);

    /* Identical gpio / synthesizer sequence, PA last */
    std::vector<Event> rf = rfEvents();
    REQUIRE(rf.size() == keyingPrefix.size() + 1);
    for (size_t i = 0; i < keyingPrefix.size(); i++) {
        INFO("event " << i);
        REQUIRE(rf[i] == keyingPrefix[i]);
    }
    REQUIRE(rf.back() == gpioSet(PIN_TX_PWR_EN));
    REQUIRE(indexOf({ Ev::PLL_LOCK, 1, 0 }) < indexOf(gpioSet(PIN_TX_PWR_EN)));

    /*
     * No analog TX (0x60 bit 7, mic path 0xE0), no register write at all:
     * the modem is armed by the opMode handler through dmrbb_startTx().
     */
    REQUIRE(analogTxStarted() == false);
    for (const auto &e : events)
        REQUIRE(e.type != Ev::REG_WRITE);
    REQUIRE(uspiOtherWrites == 0);

    /* The RF driver does not touch the modem when keying */
    REQUIRE(has({ Ev::DMRBB_IDLE, 0, 0 }) == false);
    REQUIRE(has({ Ev::DMRBB_RX, 0, 0 }) == false);
    REQUIRE(has({ Ev::DMRBB_CFG, 0, 0 }) == false);

    REQUIRE(fakeDac.DHR8R1 == CAL_PWR_HI);
}

TEST_CASE("DMR radio_enableTx never keys the PA when the PLL does not lock",
          "[radio_cs7000][dmr]")
{
    bringUp(OPMODE_DMR);
    pllLocks = false;
    radio_enableTx();

    REQUIRE(radio_getStatus() == OFF);
    REQUIRE(has(gpioSet(PIN_TX_PWR_EN)) == false);

    /* Parked on the RX local oscillator, RX VCO */
    std::vector<Event> rf = rfEvents();
    std::vector<Event> expected = {
        gpioClear(PIN_TX_PWR_EN),
        gpioClear(PIN_RX_PWR_EN),
        gpioSet(PIN_RF_APC_SW),
        gpioClear(PIN_VCOVCC_SW),
        { Ev::PLL_FREQ, 433500000, 0 },
        { Ev::PLL_LOCK, 0, 0 },
        gpioSet(PIN_VCOVCC_SW),
        { Ev::PLL_FREQ, 433500000 - 49950000, 0 },
    };
    REQUIRE(rf == expected);

    /* No analog TX stop either: nothing was started */
    for (const auto &e : events)
        REQUIRE(e.type != Ev::REG_WRITE);

    /* Retried while "transmitting" (radio_updateConfiguration path) */
    pllLocks = true;
    radio_enableTx();
    REQUIRE(radio_getStatus() == TX);
    pllLocks = false;
    events.clear();
    radio_enableTx();
    REQUIRE(radio_getStatus() == OFF);
    REQUIRE(analogTxStarted() == false);
    for (const auto &e : events)
        REQUIRE(e.type != Ev::REG_WRITE);
}

TEST_CASE("FM radio_enableTx stops the analog TX when the PLL does not lock",
          "[radio_cs7000]")
{
    bringUp(OPMODE_FM);
    radio_enableTx();
    REQUIRE(radio_getStatus() == TX);

    events.clear();
    pllLocks = false;
    radio_enableTx();
    REQUIRE(radio_getStatus() == OFF);
    REQUIRE(has(regWrite(0x60, 0x00)));
    REQUIRE(has(gpioSet(PIN_TX_PWR_EN)) == false);
}

TEST_CASE("radio_enableTx does nothing with txDisable set", "[radio_cs7000]")
{
    bringUp(OPMODE_DMR);
    rtxCfg.txDisable = 1;
    radio_enableTx();
    REQUIRE(events.empty());
    REQUIRE(radio_getStatus() == RX);
}

TEST_CASE("DMR radio_disableRtx idles the modem before dropping the PA",
          "[radio_cs7000][dmr]")
{
    bringUp(OPMODE_DMR);
    radio_enableTx();
    events.clear();

    radio_disableRtx();
    REQUIRE(radio_getStatus() == OFF);

    REQUIRE(events.size() >= 3);
    REQUIRE(events[0] == Event{ Ev::DMRBB_IDLE, 0, 0 });
    REQUIRE(events[1] == gpioClear(PIN_TX_PWR_EN));
    REQUIRE(events[2] == gpioClear(PIN_RX_PWR_EN));

    /* No analog TX stop (0x60 = 0x00 / 0xE0 / 0x34 of stopAnalogTx) */
    for (const auto &e : events)
        REQUIRE(e.type != Ev::REG_WRITE);
}

TEST_CASE("FM radio_disableRtx drops the PA then stops the analog TX",
          "[radio_cs7000]")
{
    bringUp(OPMODE_FM);
    radio_enableTx();
    events.clear();

    radio_disableRtx();
    REQUIRE(events[0] == gpioClear(PIN_TX_PWR_EN));
    REQUIRE(events[1] == gpioClear(PIN_RX_PWR_EN));
    REQUIRE(has(regWrite(0x60, 0x00)));
    REQUIRE(has({ Ev::DMRBB_IDLE, 0, 0 }) == false);
}

TEST_CASE("DMR radio_enableRx drops the PA, retunes, then starts the modem RX",
          "[radio_cs7000][dmr]")
{
    bringUp(OPMODE_DMR);
    radio_enableTx();
    events.clear();

    radio_enableRx();
    REQUIRE(radio_getStatus() == RX);

    REQUIRE(events[0] == gpioClear(PIN_TX_PWR_EN));
    REQUIRE(has(gpioSet(PIN_VCOVCC_SW)));
    REQUIRE(has({ Ev::PLL_FREQ, 433500000 - 49950000, 0 }));
    REQUIRE(has(gpioSet(PIN_RX_PWR_EN)));
    REQUIRE(events.back() == Event{ Ev::DMRBB_RX, 0, 0 });
    REQUIRE(indexOf(gpioSet(PIN_RX_PWR_EN)) < indexOf({ Ev::DMRBB_RX, 0, 0 }));

    /* The FM RX filter register 0x34 is not touched in DMR */
    for (const auto &e : events)
        REQUIRE(e.type != Ev::REG_WRITE);
}

TEST_CASE("radio_updateConfiguration feeds 0x45/0x46 from the digital pair",
          "[radio_cs7000][dmr]")
{
    bringUp(OPMODE_DMR);
    radio_updateConfiguration();

    REQUIRE(has(regWrite(0x45, CAL_DIG_Q)));
    REQUIRE(has(regWrite(0x46, CAL_DIG_I)));
    REQUIRE(has(regWrite(0x46, CAL_ANA_I)) == false);

    /* FM keeps mixing the analog I with the digital Q */
    bringUp(OPMODE_FM);
    radio_updateConfiguration();
    REQUIRE(has(regWrite(0x45, CAL_DIG_Q)));
    REQUIRE(has(regWrite(0x46, CAL_ANA_I)));
}

TEST_CASE("radio_updateConfiguration while transmitting in DMR re-keys",
          "[radio_cs7000][dmr]")
{
    bringUp(OPMODE_DMR);
    radio_enableTx();
    events.clear();

    rtxCfg.txFrequency = 438000000;
    radio_updateConfiguration();
    REQUIRE(radio_getStatus() == TX);

    /* Unkeyed before the retune, keyed again after the lock */
    std::vector<Event> rf = rfEvents();
    REQUIRE(rf[0] == gpioClear(PIN_TX_PWR_EN));
    REQUIRE(indexOf({ Ev::PLL_FREQ, 438000000, 0 })
            < indexOf({ Ev::PLL_LOCK, 1, 0 }));
    REQUIRE(indexOf({ Ev::PLL_LOCK, 1, 0 }) < indexOf(gpioSet(PIN_TX_PWR_EN)));
    REQUIRE(analogTxStarted() == false);
}

TEST_CASE("radio_setOpmode configures the modem from the rtx status",
          "[radio_cs7000][dmr]")
{
    resetAll();
    rtxCfg.opMode = OPMODE_DMR;
    rtxCfg.dmr_polite = 0;
    rtxCfg.dmr_txColorCode = 7;
    radio_init(&rtxCfg);
    events.clear();

    radio_setOpmode(OPMODE_DMR);
    REQUIRE(has({ Ev::DMRBB_CFG, 0, 0 }));
    REQUIRE(lastBbConfig.colorCode == 7);
    REQUIRE(lastBbConfig.timeslot == 2);
    REQUIRE(lastBbConfig.ownId == 2345678);
    REQUIRE(lastBbConfig.polite == false);
    REQUIRE(lastBbConfig.modeReg == DMRBB_MODE_REG_DEFAULT);

    /* Back to FM: the modem driver is terminated before fmMode() */
    events.clear();
    radio_setOpmode(OPMODE_FM);
    REQUIRE(events[0] == Event{ Ev::DMRBB_TERM, 0, 0 });
    REQUIRE(has(regWrite(0x10, 0x80)));
}
