/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_CALLCONTROLLER_H
#define DMR_CALLCONTROLLER_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include "interfaces/dmr_baseband.h"
#include "protocols/DMR/Constants.hpp"
#include "protocols/DMR/ModemPort.hpp"

namespace DMR
{

/**
 * Consumer and producer of the AMBE+2 voice payloads: the call controller
 * hands over every received voice burst and asks for the payload of every
 * own voice burst. Implemented by the DMR audio layer; when absent, received
 * voice is dropped and the AMBE silence pattern is transmitted.
 */
class VoiceSink
{
public:
    virtual ~VoiceSink() = default;

    /**
     * A voice burst of the current call was received.
     *
     * @param ambe: 27 bytes, three AMBE+2 frames.
     */
    virtual void rxVoiceFrame(const uint8_t ambe[VOICE_PAYLOAD_BYTES]) = 0;

    /**
     * The payload of the next own voice burst is needed.
     *
     * @param ambe: buffer for 27 bytes.
     * @return true when the buffer was filled, false to transmit silence.
     */
    virtual bool txVoiceFrameNeeded(uint8_t ambe[VOICE_PAYLOAD_BYTES]) = 0;
};

/**
 * DMR Tier II voice call controller: the Layer-2 slot state machine of one
 * logical channel (colour code + timeslot) driven by the timeslot and
 * system events of the modem and by the PTT, producing register writes on a
 * ModemPort and a status report for the UI.
 *
 * Event order per slot: onTimeslot() at the TS interrupt (program the next
 * slot within t2 = 27 ms), onSysEvent() at the SYS interrupt 4 ms after the
 * slot end (decode what was received), onTimeout() when the modem is silent.
 * When a single snapshot carries both events, onTimeslot() goes first.
 *
 * Timekeeping is external: every input carries the current time in
 * milliseconds; the object never sleeps, allocates or throws.
 *
 * Hardware assumptions that are UNVERIFIED on hardware are marked in the
 * implementation; each is a one-line change.
 */
class CallController
{
public:
    enum State : uint8_t {
        OFF = 0,
        RX_SEARCH, /* startRx() done, no timeslot interrupt yet          */
        RX_IDLE,   /* slots ticking, no call                             */
        RX_CALL,   /* a matching call is open, voice bursts forwarded    */
        RX_HANG,   /* call ended, kept on screen for T_CallHt            */
        TX_ARM,    /* PTT accepted, waiting for timing / own slot        */
        TX_WAKEUP, /* sending BS_Dwn_Act and waiting for the repeater    */
        TX_HEADER, /* sending the voice LC header (three times)          */
        TX_VOICE,  /* sending voice bursts A..F                          */
        TX_TERM,   /* terminator programmed, waiting for it to go out    */
    };

    /*
     * Timeouts fixed by the protocol or by OpenGD77 practice.
     */
    static constexpr uint32_t TS_WATCHDOG_MS = 200;     /* re-run startRx */
    static constexpr uint32_t TS_RECONFIGURE_MS = 2000; /* ask reconfig   */
    static constexpr uint32_t VOICE_TIMEOUT_MS = 2 * SUPERFRAME_MS;
    static constexpr uint32_t CARRIER_TIMEOUT_MS = 500; /* BS sync age    */
    static constexpr uint8_t TIMECODE_LOCK_COUNT = 5;   /* agreeing TS    */
    static constexpr uint8_t TIMECODE_DROP_COUNT = 4;   /* disagreeing TS */
    static constexpr uint8_t HEADER_REPEAT = 3;

    struct Config {
        uint32_t ownId;         /* own DMR ID, 0 = TX refused          */
        uint32_t dstId;         /* talkgroup (GROUP) or target (PRIVATE) */
        uint8_t callType;       /* dmrContactType_t GROUP/PRIVATE/ALL  */
        uint8_t colorCode;      /* 0-15                                */
        uint8_t timeslot;       /* 1 or 2                              */
        uint8_t monitor;        /* 0 CC+TS+address, 1 CC+TS any address,
                                   2 anything                          */
        bool polite;            /* channel access polite to own CC     */
        bool repeater;          /* repeater channel: TX in RMO, waking
                                   the repeater up when it is silent   */
        uint16_t hangTimeMs;    /* T_CallHt                            */
        uint8_t wakeupAttempts; /* N_Wakeup                            */
        uint16_t syncWuMs;      /* T_SyncWu                            */
        uint32_t txTimeoutMs;   /* T_TO, 0 = disabled                  */
    };

    struct Report {
        uint8_t state;           /* CallController::State              */
        uint8_t callState;       /* enum dmrCallState of rtx.h         */
        uint8_t slotLock;        /* 0 none, 1 slots ticking, 2 timecode
                                    locked                             */
        uint8_t timecode;        /* local timecode of the current slot */
        bool lcOk;               /* rxSrc/rxDst/rxFlco hold a valid LC */
        bool lcManufacturer;     /* LC with a manufacturer FID: shown,
                                    voice not forwarded                */
        uint32_t rxSrc;          /* source address of the call         */
        uint32_t rxDst;          /* destination address of the call    */
        uint8_t rxFlco;          /* FLCO of the call                   */
        uint8_t rxColorCodeSeen; /* colour code of the last burst      */
        uint8_t rxTimeslot;      /* timeslot of the call, 1 or 2       */
        uint8_t rxSyncType;      /* 0 MS/DMO, 1 BS/RMO                 */
        bool hangActive;         /* T_CallHt running                   */
        bool noId;               /* marker: PTT refused, no DMR ID     */
        bool busy;               /* marker: TX rejected, channel busy  */
        bool wakeupFailed;       /* marker: repeater did not answer    */
        bool needsReconfigure;   /* no timeslot interrupt for 2 s      */
    };

    CallController();

    /**
     * Attach the modem port. Must be done before enable().
     *
     * @param port: modem port, owned by the caller.
     */
    void setPort(ModemPort *port);

    /**
     * Attach the voice sink, optional.
     *
     * @param sink: voice sink, owned by the caller, or nullptr.
     */
    void setVoiceSink(VoiceSink *sink);

    /**
     * Apply a configuration. Takes effect at the next event; re-run
     * enable() after changing the colour code or timeslot so that the
     * modem is reprogrammed by the caller.
     *
     * @param cfg: new configuration.
     */
    void configure(const Config &cfg);

    /**
     * @return current configuration.
     */
    const Config &config() const
    {
        return cfg;
    }

    /**
     * Start: reset all the state, start reception (OFF -> RX_SEARCH).
     *
     * @param nowMs: current time.
     */
    void enable(uint32_t nowMs);

    /**
     * Stop: idle the modem from any state (-> OFF).
     */
    void disable();

    /**
     * PTT input. A press is accepted in the RX states when an own ID is
     * configured; otherwise the noId marker is set. A press is ignored
     * until the PTT is released again after a refused, rejected or timed
     * out transmission.
     *
     * @param ptt: PTT state.
     * @param nowMs: current time.
     */
    void setPtt(bool ptt, uint32_t nowMs);

    /**
     * Timeslot interrupt: track the timecode from register 0x52 and program
     * the next slot.
     *
     * @param s: snapshot with r52 valid.
     * @param nowMs: current time.
     */
    void onTimeslot(const struct dmrbbSnapshot &s, uint32_t nowMs);

    /**
     * System interrupt: decode the flags of register 0x82 and what was
     * received.
     *
     * @param s: snapshot with irq82, r51, r52, r5f, lc and voice valid.
     * @param nowMs: current time.
     */
    void onSysEvent(const struct dmrbbSnapshot &s, uint32_t nowMs);

    /**
     * No event within the wait: run the timers and the timeslot watchdog.
     *
     * @param nowMs: current time.
     */
    void onTimeout(uint32_t nowMs);

    /**
     * Clear the top-bar markers (noId, busy, wakeupFailed).
     */
    void clearMarkers();

    /**
     * @return the status report, updated after every input.
     */
    const Report &report() const
    {
        return rep;
    }

    /**
     * @return current state.
     */
    State state() const
    {
        return st;
    }

private:
    enum WakeupPhase : uint8_t {
        WU_SEND = 0, /* waiting for the own slot to send the CSBK     */
        WU_SENT,     /* CSBK programmed, going back to RX next slot   */
        WU_WAIT,     /* in RX, waiting for the repeater's sync        */
    };

    bool isTx() const;
    bool carrierPresent(uint32_t nowMs) const;
    uint8_t slotOf(const struct dmrbbSnapshot &s) const;
    bool burstIsCall(const struct dmrbbSnapshot &s) const;
    bool colorCodeAccepted(uint8_t cc) const;
    bool slotAccepted(uint8_t slot) const;

    void resetTiming();
    void trackTimecode(uint8_t rxTc);
    void runTimers(uint32_t nowMs);
    void publish();

    void goRx(uint32_t nowMs, State next);
    void enterHang(uint32_t nowMs);
    bool tryOpenCall(const struct dmrbbSnapshot &s, uint32_t nowMs);
    void processControlFrame(const struct dmrbbSnapshot &s, uint32_t nowMs);

    void enterTxArm(uint32_t nowMs);
    void armTx(bool activeTiming);
    void enterWakeup(uint32_t nowMs);
    void abortTx(uint32_t nowMs, bool busy);
    void buildTxLc();
    void programHeader();
    void programVoice();
    void programTerminator();
    void txSlot(uint32_t nowMs, bool ownSlot);

    ModemPort *port;
    VoiceSink *sink;
    Config cfg;
    Report rep;
    State st;

    /* Timing */
    uint8_t timecode;  /* local timecode of the slot that just started  */
    bool tcValid;      /* timecode initialised from the first TS        */
    uint8_t agree;     /* consecutive agreeing TS                       */
    uint8_t disagree;  /* consecutive disagreeing TS                    */
    uint8_t slotLock;  /* 0 none, 1 slots ticking, 2 timecode locked    */
    uint8_t lastSync;  /* last received sync type, R5F_SYNC_*           */
    uint32_t lastTsMs; /* time of the last TS interrupt                 */
    uint32_t lastBsSyncMs;
    bool wdRestarted;  /* startRx re-run by the watchdog since last TS  */
    bool skipLc;       /* ignore the first LC read after a mode switch  */

    /* Reception */
    uint8_t callSlot;     /* 1, 2 or 0 when unknown (MS sync)           */
    uint8_t callTimecode; /* local timecode of the call's slot          */
    bool callDmo;         /* call has no BS sync: gate the other slot   */
    uint32_t lastVoiceMs;
    uint32_t hangStartMs;

    /* Transmission */
    bool ptt;
    bool pttLatched;   /* ignore PTT until released                    */
    bool activeTiming; /* DMO (true) or RMO (false)                    */
    bool armed;        /* 0x21/0x40 written, waiting for the own slot  */
    bool txTimedOut;   /* T_TO expired, finish the superframe          */
    uint8_t headers;   /* headers programmed so far                    */
    uint8_t seq;       /* next voice burst, 0 = A .. 5 = F             */
    uint8_t wakeups;   /* wake-up attempts done                        */
    uint8_t wakeupPhase;
    uint32_t armStartMs;
    uint32_t txStartMs;
    uint32_t wakeupSentMs;
    uint8_t txLc[CHIP_LC_BYTES];
    uint8_t voiceBuf[VOICE_PAYLOAD_BYTES];
};

} /* namespace DMR */

#endif /* DMR_CALLCONTROLLER_H */
