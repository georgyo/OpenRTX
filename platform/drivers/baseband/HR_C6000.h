/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HRC6000_H
#define HRC6000_H

#include "core/datatypes.h"
#include "drivers/baseband/HR_Cx000.h"

enum class C6000_SpiOpModes : uint8_t
{
    AUX    = 1,     ///< Auxiliary configuration registers.
    DATA   = 2,     ///< Write TX data register and read RX data register.
    SOUND  = 3,     ///< Voice prompt sample register.
    CONFIG = 4,     ///< Main configuration registers.
    AMBE3K = 5,     ///< AMBE3000 configuration register.
    DATA_R = 6,     ///< Write RX data register and read TX data register.
    AMBE1K = 7      ///< AMBE1000 configuration register.
};

class HR_C6000 : public HR_Cx000 < C6000_SpiOpModes >
{
public:

    /**
     * Constructor.
     *
     * @param uSpi: pointer to SPI device for "user" SPI interface.
     * @param uCs: gpioPin object for "user" SPI chip select.
     */
    HR_C6000(const struct spiDevice *uSpi, const struct gpioPin uCs) :
        HR_Cx000< C6000_SpiOpModes >(uSpi, uCs) { }

    /**
     * Configure CTCSS tone transmission.
     *
     * @param tone: CTCSS tone frequency.
     * @param deviation: CTCSS tone deviation.
     */
    void setTxCtcss(const tone_t tone, const uint8_t deviation);

    /**
     * Configure CTCSS tone detection.
     *
     * @param tone: CTCSS tone frequency.
     */
    void setRxCtcss(const tone_t tone);

    /**
     * Test if RX CTCSS tone has been detected.
     *
     * @return true if RX CTCSS tone has been detected.
     */
    inline bool ctcssDetected()
    {
        uint8_t reg = readCfgRegister(0x93);
        return ((reg & 0x01) != 0) ? true : false;
    }

    /**
     * Disable all tone encode and decode.
     */
    inline void disableTones()
    {
        writeCfgRegister(0xA1, 0x00);     // Disable all tones
    }

    /**
     * Transmit a tone of a given frequency.
     *
     * @param freq: tone frequency in Hz.
     * @param deviation: tone deviation.
     */
    void sendTone(const uint32_t freq, const uint8_t deviation);

    /**
     * Get the current value of a configuration register with 16-bit address.
     *
     * @param addr: register address.
     * @return current value of the register.
     */
    inline uint8_t readCfgRegister16(const uint16_t addr)
    {
        return readReg16(C6000_SpiOpModes::CONFIG, addr);
    }

    /**
     * Set the value of a configuration register with 16-bit address.
     *
     * @param addr: register address.
     * @param value: new register value.
     */
    inline void writeCfgRegister16(const uint16_t addr, const uint8_t value)
    {
        writeReg16(C6000_SpiOpModes::CONFIG, addr, value);
    }

    /**
     * Raw register access by "user" SPI page, for the DMR monitor screen:
     * page 1 AUX, 2 RX RAM (read) / TX RAM (write), 4 CONFIG, 6 TX RAM (read)
     * / RX RAM (write). See C6000_SpiOpModes.
     *
     * @param page: SPI opcode of the page.
     * @param addr: register address.
     * @return current value of the register.
     */
    inline uint8_t readPageRegister(const uint8_t page, const uint8_t addr)
    {
        return readReg(static_cast< C6000_SpiOpModes >(page & 0x07), addr);
    }

    /**
     * Raw register write by "user" SPI page, see readPageRegister().
     *
     * @param page: SPI opcode of the page.
     * @param addr: register address.
     * @param value: new register value.
     */
    inline void writePageRegister(const uint8_t page, const uint8_t addr,
                                  const uint8_t value)
    {
        writeReg(static_cast< C6000_SpiOpModes >(page & 0x07), addr, value);
    }

    /*
     * DMR helpers. Register numbers and bit maps from the HR_C6000 User
     * Manual (OpenGD77 team translation) chapter 8 as transcribed by G4EML;
     * none of them has been run on a CS7000 yet.
     */

    /**
     * Set the colour code of the transmitted bursts and of the chip-side
     * colour code filter: register 0x1F "LocalEMB", bits [7:4] colour code,
     * bit 3 PI = 0, bits [1:0] encryption = 0 (G4EML table, manual §5.4.5).
     *
     * @param cc: colour code, 0 to 15.
     */
    inline void setColorCode(const uint8_t cc)
    {
        writeCfgRegister(0x1F, (cc & 0x0F) << 4);
    }

    /**
     * Set the local (own) DMR ID, registers 0x14, 0x15, 0x16 "LocalSrcAddress"
     * low, mid and high byte in this order (G4EML table).
     * UNVERIFIED on hardware: the byte order. The G4EML table says L/M/H,
     * the MD-3x0 OEM capture looks H/M/L. Harmless either way because the
     * address matching is done on the MCU, not by the chip.
     *
     * @param id: 24-bit DMR ID.
     */
    inline void setLocalAddress(const uint32_t id)
    {
        writeCfgRegister(0x14, id & 0xFF);          // Low byte
        writeCfgRegister(0x15, (id >> 8) & 0xFF);   // Mid byte
        writeCfgRegister(0x16, (id >> 16) & 0xFF);  // High byte
    }

    /**
     * Set the channel access policy, registers 0x20 and 0x21 (G4EML table:
     * 2-bit fields, 00 impolite, 01 polite to all, 10 polite to own colour
     * code). 0x20 = 0x00 and 0x21 = 0xA0 is the "polite to own colour code"
     * setting observed in OpenGD77, 0x21 = 0x00 is impolite.
     *
     * @param polite: true for polite to own colour code, false for impolite.
     */
    inline void setAccessPolicy(const bool polite)
    {
        writeCfgRegister(0x20, 0x00);
        writeCfgRegister(0x21, polite ? 0xA0 : 0x00);
    }

    /**
     * Set the Layer-2 interrupt mask, register 0x81 "InterClass1Mask" for the
     * flags of register 0x82 (manual §5.1.1).
     * UNVERIFIED on hardware: the polarity, taken from fmMode() writing
     * 0x81 = 0x00 as "all interrupts masked".
     *
     * @param mask: mask bits, 1 enables the corresponding interrupt.
     */
    inline void setIrqMask(const uint8_t mask)
    {
        writeCfgRegister(0x81, mask);
    }

    /**
     * Read the Layer-2 interrupt flags, register 0x82 (manual §5.1.1).
     *
     * @return the interrupt flags.
     */
    inline uint8_t readIrqFlags()
    {
        return readCfgRegister(0x82);
    }

    /**
     * Acknowledge Layer-2 interrupt flags: register 0x83 clears the bits of
     * 0x82 which are set in the written value (manual §5.1.1).
     *
     * @param flags: flags to be cleared.
     */
    inline void clearIrq(const uint8_t flags)
    {
        writeCfgRegister(0x83, flags);
    }

    /*
     * Register 0x82 flag bits, manual §5.1.1 and G4EML table: bit 7 TX
     * request rejected (channel busy per the 0x20/0x21 access policy), bit 6
     * TX started (sub-status 0x84, bit 7 = voice TX start), bit 5 TX ended
     * (sub-status 0x86, bit 7 = voice and data TX complete), bit 4 late
     * entry into an ongoing call, bit 3 control frame parsed (LC/CSBK/EMB
     * in RX RAM), bit 2 service data message (sub-status 0x90), bit 1
     * abnormal voice exit (sub-status 0x98), bit 0 PHY-only receive.
     */
    enum Irq82 : uint8_t
    {
        IRQ_TX_REJECTED   = 0x80,
        IRQ_TX_START      = 0x40,
        IRQ_TX_END        = 0x20,
        IRQ_LATE_ENTRY    = 0x10,
        IRQ_RX_DATA       = 0x08,
        IRQ_RX_INFO       = 0x04,
        IRQ_ABNORMAL_EXIT = 0x02,
        IRQ_PHY_ONLY      = 0x01
    };

    /*
     * Sub-status bits read with the TX flags: register 0x84 bit 7 "voice
     * TX start" and register 0x86 bit 7 "TX complete, voice and data"
     * (G4EML table). UNVERIFIED on hardware: the remaining bits of both
     * registers describe the external AMBE3000 hook-up and data
     * retransmission, which this driver does not use.
     */
    static constexpr uint8_t SUB84_VOICE_TX_START = 0x80;
    static constexpr uint8_t SUB86_TX_COMPLETE    = 0x80;

    /*
     * Register 0x86 bit 2 "Short LC received" (manual §5.1.1 register 0x82
     * bit 5 description, G4EML "ShortLC"): the CACH Short LC is then read
     * from RX RAM 0x24-0x28, the address of the TX-side allocation (manual
     * Table 5.2) assumed to be mirrored by the RX-side RAM, whose table
     * (5.3) is omitted in the translation. UNVERIFIED on hardware: the RX
     * address, and whether the flag fires on a receiving radio, the manual
     * lists it among the end-of-transmission sub-status bits.
     */
    static constexpr uint8_t SUB86_SHORT_LC       = 0x04;

    /**
     * Test the "TX request rejected" flag of a register 0x82 value: the
     * chip refused to transmit because the channel is busy under the
     * configured access policy (0x21 = 0xA2, polite to own colour code).
     *
     * @param irq82: value read from register 0x82.
     * @return true if the transmission request was rejected.
     */
    static inline bool txRejected(const uint8_t irq82)
    {
        return (irq82 & IRQ_TX_REJECTED) != 0;
    }

    /**
     * Test the "TX started" flag of a register 0x82 value, register 0x84
     * carries the sub-status.
     *
     * @param irq82: value read from register 0x82.
     * @return true if a transmission started.
     */
    static inline bool txStarted(const uint8_t irq82)
    {
        return (irq82 & IRQ_TX_START) != 0;
    }

    /**
     * Test the "TX ended" flag of a register 0x82 value, register 0x86
     * carries the sub-status.
     *
     * @param irq82: value read from register 0x82.
     * @return true if a transmission ended.
     */
    static inline bool txEnded(const uint8_t irq82)
    {
        return (irq82 & IRQ_TX_END) != 0;
    }

    /**
     * Arm DMR transmission: register 0x21 = 0xA2 (polite to own colour
     * code, bit 1 "clear the vocoder encode buffer", self-clearing) or 0x02
     * (impolite), then register 0x40 = 0xE3 (TxEn + RxEn, active timing:
     * direct mode, the chip generates the 30ms time axis) or 0xC3 (TxEn +
     * RxEn, passive timing: repeater mode, bursts aligned to the received
     * sync). Nothing is radiated until register 0x41 = 0x80 selects a TX
     * slot. G4EML table and OpenGD77 values; UNVERIFIED on hardware: manual
     * §5.4.5 arms with 0x40 = 0xA3 (TxEn without RxEn).
     *
     * @param activeTiming: true for direct mode, false for repeater mode.
     * @param polite: true for polite to own colour code, false for impolite.
     */
    inline void dmrTxStart(const bool activeTiming, const bool polite)
    {
        writeCfgRegister(0x21, polite ? 0xA2 : 0x02);
        writeCfgRegister(0x40, activeTiming ? 0xE3 : 0xC3);
    }

    /**
     * Start DMR reception with passive timing: register 0x40 = 0xC3 (TxEn +
     * RxEn, timing derived from the received sync), then 0x41 = 0x20
     * (SyncFail: search a new sync) and 0x41 = 0x50 (RxNxtSlotEn +
     * begin_v_layer2, received voice routed to the vocoder port).
     * UNVERIFIED on hardware: manual §5.4.5 gives 0x40 = 0x43 / 0x41 = 0x40
     * as the minimal form, the values here are the OpenGD77 ones.
     */
    inline void dmrRxStart()
    {
        writeCfgRegister(0x40, 0xC3);
        writeCfgRegister(0x41, 0x20);
        writeCfgRegister(0x41, 0x50);
    }

    /**
     * Stop DMR transmission and reception: register 0x40 = 0x03 (TxEn and
     * RxEn cleared, non-test mode). The slot interrupts stop.
     */
    inline void dmrIdle()
    {
        writeCfgRegister(0x40, 0x03);
    }

private:

    /**
     * Write a register with 16-bit address.
     *
     * @param opMode: "operating mode" specifier, see datasheet for details.
     * @param addr: register address.
     * @param value: value to be written.
     */
    void writeReg16(const C6000_SpiOpModes opMode, const uint16_t addr, const uint8_t value)
    {
        uint8_t data[4];

        data[0] = SPI_FLAGS_EXTD | static_cast< uint8_t >(opMode);
        data[2] = (addr >> 8) & 0x07;
        data[1] = addr & 0xFF;
        data[3] = value;

        ScopedChipSelect cs(uSpi, uCs);
        spi_send(uSpi, data, 4);
    }

    /**
     * Read a register with 16-bit address.
     *
     * @param opMode: "operating mode" specifier, see datasheet for details.
     * @param addr: register address.
     * @return: value read from the register.
     */
    uint8_t readReg16(const C6000_SpiOpModes opMode, const uint16_t addr)
    {
        uint8_t data[3];
        uint8_t value[1];

        data[0] = SPI_FLAGS_READ | SPI_FLAGS_EXTD
                | static_cast< uint8_t >(opMode);
        data[2] = (addr >> 8) & 0x07;
        data[1] = addr & 0xFF;

        ScopedChipSelect cs(uSpi, uCs);
        spi_send(uSpi, data, sizeof(data));
        spi_receive(uSpi, value, 1);

        return value[0];
    }
};

#endif /* HRC6000_H */
