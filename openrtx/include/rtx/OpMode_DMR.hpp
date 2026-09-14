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
 * Placeholder for the DMR audio layer of a later stage: received voice
 * payloads are counted and dropped, own voice bursts carry the AMBE silence
 * pattern (the controller transmits it when no payload is provided). No
 * thread, no buffer: the audio paths are requested by OpMode_DMR so that the
 * amplifier and microphone gating already behave as they will with voice.
 */
class DmrAudioStub : public DMR::VoiceSink
{
public:
    DmrAudioStub() : rxFrames(0), txRequests(0)
    {
    }

    void rxVoiceFrame(const uint8_t ambe[DMR::VOICE_PAYLOAD_BYTES]) override
    {
        (void)ambe;
        rxFrames++;
    }

    bool txVoiceFrameNeeded(uint8_t ambe[DMR::VOICE_PAYLOAD_BYTES]) override
    {
        (void)ambe;
        txRequests++;
        return false;
    }

    void reset()
    {
        rxFrames = 0;
        txRequests = 0;
    }

    uint32_t rxFrames;   /**< Voice bursts received since reset()     */
    uint32_t txRequests; /**< Voice bursts requested since reset()    */
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
     * How long the noId / busy / wakeupFailed markers stay raised, unless a
     * new PTT press clears them first.
     */
    static constexpr uint32_t MARKER_TIME_MS = 3000;

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
     * @return the audio placeholder, for the tests.
     */
    const DmrAudioStub &audio() const
    {
        return audioStub;
    }

    /**
     * @return true when the modem driver initialised successfully.
     */
    bool modemReady() const
    {
        return ready;
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
     * Request and release the audio paths, drive the RF stage and the LEDs
     * according to the controller state.
     *
     * @param nowMs: current time.
     */
    void followState(uint32_t nowMs);

    /**
     * Copy the controller's report into the RTX status.
     *
     * @param status: RTX status to update.
     */
    void publish(rtxStatus_t *const status);

    DmrModemAdapter port;            ///< Modem port towards dmr_baseband.h
    DmrAudioStub audioStub;          ///< Audio placeholder
    DMR::CallController ctrl;        ///< Slot state machine
    DMR::CallController::Config cfg; ///< Controller configuration
    struct dmrbbConfig bbCfg;        ///< Modem configuration
    struct dmrbbSnapshot snap;       ///< Last modem event
    pathId rxAudioPath;              ///< Audio path ID for RX
    pathId txAudioPath;              ///< Audio path ID for TX
    bool ready;                      ///< Modem driver initialised
    bool enabled;                    ///< enable() done, disable() not yet
    bool cfgValid;                   ///< cfg/bbCfg built at least once
    bool cfgPending;                 ///< Modem reconfiguration deferred (TX)
    bool markerRaised;               ///< A clearable marker is shown
    uint32_t markerMs;               ///< When the marker was raised
    uint8_t rfState;                 ///< RfState of the RF stage
    uint8_t markers;                 ///< Markers published to the UI
};

#endif /* OPMODE_DMR_H */
