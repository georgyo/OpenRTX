/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DMR_H
#define OPMODE_DMR_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include "core/audio_path.h"
#include "core/dmr_audio.h"
#include "interfaces/dmr_baseband.h"
#include "protocols/DMR/CallController.hpp"
#include "protocols/DMR/ModemPort.hpp"
#include "OpMode.hpp"

/**
 * Modem port of the firmware: forwards the call controller's register-level
 * requests to the dmr_baseband.h driver of the target. The compound
 * operations use the driver's own sequences instead of the ModemPort
 * defaults, so that a target driver can implement them as it sees fit.
 */
class DmrModemAdapter : public DMR::ModemPort
{
public:
    void setNextSlot(uint8_t r41) override
    {
        dmrbb_setNextSlot(r41);
    }

    void setTxFrameType(uint8_t r50) override
    {
        dmrbb_setTxFrameType(r50);
    }

    void setMode(uint8_t r40) override
    {
        /*
         * Only the compound operations below reach the driver: the driver
         * owns the register 0x40 sequences (dmrbb_startRx, dmrbb_startTx,
         * dmrbb_idle), a bare mode write is never needed by the controller.
         */
        (void)r40;
    }

    void setAccess(uint8_t r21) override
    {
        /*
         * The access policy is part of the driver's dmrbb_startTx() sequence,
         * derived from dmrbbConfig::polite, see startTx() below.
         */
        (void)r21;
    }

    void writeTxLc(const uint8_t lc[DMR::CHIP_LC_BYTES]) override
    {
        dmrbb_writeTxLc(lc);
    }

    void writeVoice(const uint8_t ambe[DMR::VOICE_PAYLOAD_BYTES]) override
    {
        dmrbb_writeVoice(ambe);
    }

    void startRx() override
    {
        dmrbb_startRx();
    }

    void startTx(bool activeTiming) override
    {
        dmrbb_startTx(activeTiming);
    }

    void idle() override
    {
        dmrbb_idle();
    }
};

/**
 * Voice sink of the call controller: the bridge to the DMR audio layer
 * (core/dmr_audio.h). Received payloads are pushed to the playback queue,
 * own voice bursts take their payload from the encoder queue; when the audio
 * thread is not running (no path yet, path suspended, no microphone on the
 * emulator) received voice is dropped and the controller transmits the AMBE
 * silence pattern. The counters are for the tests and the monitor screen.
 */
class DmrAudioLink : public DMR::VoiceSink
{
public:
    DmrAudioLink()
        : rxFrames(0)
        , rxQueued(0)
        , txRequests(0)
        , txFrames(0)
        , txFlushes(0)
        , txUnderruns(0)
    {
    }

    void rxVoiceFrame(const uint8_t ambe[DMR::VOICE_PAYLOAD_BYTES]) override
    {
        rxFrames++;
        if (dmrAudio_pushFrame(ambe))
            rxQueued++;
    }

    /*
     * One payload per own voice burst: taken from the encoder queue when
     * there is one, otherwise the controller sends the AMBE silence
     * pattern. Nothing queued while the encoder thread runs is an underrun
     * (the microphone stream fell behind the 60 ms cadence); with no
     * encoder at all (path refused, no microphone on the emulator) every
     * burst is silence by design and not counted as such.
     */
    bool txVoiceFrameNeeded(uint8_t ambe[DMR::VOICE_PAYLOAD_BYTES]) override
    {
        txRequests++;
        if (dmrAudio_popFrame(ambe)) {
            txFrames++;
            return true;
        }

        if (dmrAudio_running())
            txUnderruns++;

        return false;
    }

    /**
     * The voice bursts of a transmission are about to begin: drop the
     * payloads encoded while the headers went out, but the newest one.
     */
    void startVoice()
    {
        txFlushes++;
        dmrAudio_flushTx();
    }

    void reset()
    {
        rxFrames = 0;
        rxQueued = 0;
        txRequests = 0;
        txFrames = 0;
        txFlushes = 0;
        txUnderruns = 0;
    }

    uint32_t rxFrames;    /**< Voice bursts received since reset()         */
    uint32_t rxQueued;    /**< Of which taken by the audio layer           */
    uint32_t txRequests;  /**< Voice bursts requested since reset()        */
    uint32_t txFrames;    /**< Of which filled by the audio layer          */
    uint32_t txFlushes;   /**< Transmissions whose voice bursts began      */
    uint32_t txUnderruns; /**< Silence sent while the encoder was running  */
};

/**
 * Specialisation of the OpMode class for the management of the DMR operating
 * mode: glue between the RTX status, the DMR::CallController state machine
 * and the dmr_baseband.h modem driver of the target.
 *
 * On a target whose driver reports no DMR modem (dmrbb_init() returns -1) the
 * mode stays off, with the "not supported" marker raised for the UI.
 */
class OpMode_DMR : public OpMode
{
public:
    /**
     * How long the noId / busy / wakeupFailed / rfFailed markers stay
     * raised, unless a new PTT press clears them first.
     */
    static constexpr uint32_t MARKER_TIME_MS = 3000;

    /**
     * How long the TS1/TS2 busy hints taken from a repeater's Activity
     * Update Short LC stay raised after the last Short LC of any kind (a
     * Null message included) was received with a good CRC: a repeater
     * sends one every CACH cycle (four bursts, 120 ms) while it is up, so
     * two seconds of silence mean the carrier is gone. A retune and a
     * modem reprogramming clear them at once.
     */
    static constexpr uint32_t TS_BUSY_HOLD_MS = 2000;

    /**
     * Constructor.
     */
    OpMode_DMR();

    /**
     * Destructor.
     */
    ~OpMode_DMR();

    /**
     * Enable the operating mode: initialise the modem driver, program the
     * modem with the current configuration and start receiving.
     */
    virtual void enable() override;

    /**
     * Disable the operating mode: stop the modem, release the audio paths and
     * switch the RF stage off.
     */
    virtual void disable() override;

    /**
     * Update the internal FSM: wait for the next modem event (at most 30 ms),
     * feed it to the call controller together with the PTT state and publish
     * the controller's report into the RTX status.
     *
     * @param status: pointer to the rtxStatus_t structure containing the
     * current RTX status.
     * @param newCfg: flag used inform the internal FSM that a new RTX
     * configuration has been applied.
     */
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    /**
     * Get the mode identifier corresponding to the OpMode class.
     *
     * @return the corresponding flag from the opmode enum.
     */
    virtual opmode getID() override
    {
        return OPMODE_DMR;
    }

    /**
     * Check if RX squelch is open: true while a call is being received.
     *
     * @return true if RX squelch is open.
     */
    virtual bool rxSquelchOpen() override
    {
        return ctrl.report().callState == DMR_CALL_RX;
    }

    /**
     * @return the call controller, for the tests.
     */
    const DMR::CallController &controller() const
    {
        return ctrl;
    }

    /**
     * Set the transmit timeout T_TO (TS 102 361-2 §6.1: designer's choice
     * up to 180 s for Tier II), applied to the next transmission. The
     * default is DMR::T_TO_S; the tests shorten it, a settings item may
     * drive it later.
     *
     * @param ms: timeout in milliseconds, 0 to disable.
     */
    void setTxTimeout(uint32_t ms)
    {
        txTimeoutMs = ms;
    }

    /**
     * @return the voice sink, for the tests.
     */
    const DmrAudioLink &audio() const
    {
        return audioLink;
    }

    /**
     * @return true when the modem driver initialised successfully.
     */
    bool modemReady() const
    {
        return ready;
    }

    /**
     * @return true while the RF stage is keyed for transmission; for the
     * tests.
     */
    bool rfKeyed() const
    {
        return rfState == RF_TX;
    }

    /**
     * @return ID of the speaker audio path, -1 when not requested; for the
     * tests.
     */
    pathId rxAudioPathId() const
    {
        return rxAudioPath;
    }

    /**
     * @return ID of the microphone audio path, -1 when not requested; for
     * the tests.
     */
    pathId txAudioPathId() const
    {
        return txAudioPath;
    }

private:
    enum RfState : uint8_t { RF_OFF = 0, RF_RX, RF_TX };

    /**
     * Build the controller and modem configurations from the RTX status and
     * apply them: the modem is reprogrammed and the controller restarted only
     * when a parameter the modem holds (colour code, timeslot, own ID, access
     * policy) changed.
     *
     * @param status: current RTX status.
     * @param force: reprogram the modem regardless of what changed.
     */
    void applyConfig(const rtxStatus_t *const status, bool force);

    /**
     * Reprogram the modem with bbCfg and restart the controller, the
     * playback thread being stopped first.
     *
     * @param nowMs: current time.
     */
    void reprogramModem(uint32_t nowMs);

    /**
     * Request and release the audio paths, start and stop the audio thread
     * on them, drive the RF stage and the LEDs according to the controller
     * state.
     *
     * @param nowMs: current time.
     */
    void followState(uint32_t nowMs);

    /**
     * Key or release the RF stage as the controller's report asks: one
     * radio_enableTx() when the modem is armed, one radio_disableRtx()
     * followed by radio_enableRx() once the last burst has gone out. A
     * driver that refuses to key (PLL not locked, radio_getStatus() not
     * TX) aborts the transmission through CallController::rfFailed().
     *
     * @param key: true to transmit, false to receive.
     * @param nowMs: current time.
     */
    void keyRf(bool key, uint32_t nowMs);

    /**
     * Update the bring-up monitor block from the last modem event: the
     * interrupt counters, the registers the event refers to and the link
     * control RAM of the last header. Published by publish().
     *
     * @param ev: event mask returned by dmrbb_waitEvent().
     */
    void monitor(uint32_t ev);

    /**
     * Copy the controller's report and the monitor block into the RTX
     * status.
     *
     * @param status: RTX status to update.
     */
    void publish(rtxStatus_t *const status);

    /**
     * Feed the last heard list: one entry per received call, inserted when
     * the controller opens a call (its headers) and again when a call from
     * another source or to another destination takes its place, never per
     * burst. A call reopened after its hang time counts as a new one.
     *
     * @param nowMs: current time.
     */
    void trackCall(uint32_t nowMs);

    /**
     * Drop the TS busy hints and the monitor's Short LC: the repeater they
     * describe is no longer listened to (retune, modem reprogramming).
     */
    void forgetShortLc();

    /**
     * Parse the Short LC of a system event, when the modem reports one:
     * the 28 bits and the CRC of the snapshot (RX RAM 0x24-0x28) are
     * verified, an Activity Update (TS 102 361-2 §7.1.3.2) sets the TS1/TS2
     * busy hints, and the message and its counters go to the monitor block.
     *
     * @param nowMs: current time.
     */
    void parseShortLc(uint32_t nowMs);

    DmrModemAdapter port;            ///< Modem port towards dmr_baseband.h
    DmrAudioLink audioLink;          ///< Voice sink towards dmr_audio
    DMR::CallController ctrl;        ///< Slot state machine
    DMR::CallController::Config cfg; ///< Controller configuration
    struct dmrbbConfig bbCfg;        ///< Modem configuration
    struct dmrbbSnapshot snap;       ///< Last modem event
    dmrMonitor_t mon;                ///< Bring-up counters for the UI
    pathId rxAudioPath;              ///< Audio path ID for RX
    pathId txAudioPath;              ///< Audio path ID for TX
    bool ready;                      ///< Modem driver initialised
    bool enabled;                    ///< enable() done, disable() not yet
    bool cfgValid;                   ///< cfg/bbCfg built at least once
    bool cfgPending;                 ///< Modem reconfiguration deferred (TX)
    bool rxAudioFailed;              ///< dmrAudio_startRx() refused the path
    bool txAudioFailed;              ///< dmrAudio_startTx() refused the path
    bool txVoiceStarted;             ///< Controller seen in TX_VOICE
    bool markerRaised;               ///< A clearable marker is shown
    uint32_t markerMs;               ///< When the marker was raised
    uint32_t txTimeoutMs;            ///< T_TO for the next transmission
    uint8_t rfState;                 ///< RfState of the RF stage
    uint8_t markers;                 ///< Markers published to the UI
    uint8_t tsBusy;                  ///< TS1/TS2 busy hints, bit0 = TS1
    uint32_t tsBusyMs;               ///< When the last good Short LC arrived
    uint32_t rxFrequency;            ///< RX frequency of the configuration
    bool callTracked;                ///< Current call is in last heard
    uint32_t trackedSrc;             ///< Source of the tracked call
    uint32_t trackedDst;             ///< Destination of the tracked call
};

#endif /* OPMODE_DMR_H */
