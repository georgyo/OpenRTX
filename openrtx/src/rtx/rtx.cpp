/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/radio.h"
#include "hwconfig.h"
#include <string.h>
#include "rtx/rtx.h"
#include "rtx/OpMode_FM.hpp"
#include "rtx/OpMode_M17.hpp"
#ifdef CONFIG_DMR
#include "rtx/OpMode_DMR.hpp"
#endif

static pthread_mutex_t   *cfgMutex;     // Mutex for incoming config messages
static const rtxStatus_t *newCnf;       // Pointer for incoming config messages
static rtxStatus_t        rtxStatus;    // RTX driver status
static rssi_t             rssi;         // Current RSSI in dBm
static bool               reinitFilter; // Flag for RSSI filter re-initialisation

static OpMode  *currMode;               // Pointer to currently active opMode handler
static OpMode     noMode;               // Empty opMode handler for opmode::NONE
static OpMode_FM  fmMode;               // FM mode handler
#ifdef CONFIG_M17
static OpMode_M17 m17Mode;              // M17 mode handler
#endif
#ifdef CONFIG_DMR
static OpMode_DMR dmrMode;              // DMR mode handler
#endif

/**
 * \internal
 * DMR receive report fields of rtxStatus_t. These are owned by the DMR opMode
 * handler, which updates them on every update() pass: a configuration pushed
 * by the UI thread must not clear them. Kept small on purpose, it lives on the
 * stack of the rtx thread.
 */
struct DmrReport
{
    bool     lcOk;
    uint32_t rxSrcId;
    uint32_t rxDstId;
    uint8_t  rxFlco;
    uint8_t  rxColorCodeSeen;
    uint8_t  rxTimeslot;
    uint8_t  rxSyncType;
    uint8_t  callState;
    uint8_t  slotLock;
    uint8_t  markers;
};

static void saveDmrReport(DmrReport *report, const rtxStatus_t *status)
{
    report->lcOk            = status->dmr_lcOk;
    report->rxSrcId         = status->dmr_rxSrcId;
    report->rxDstId         = status->dmr_rxDstId;
    report->rxFlco          = status->dmr_rxFlco;
    report->rxColorCodeSeen = status->dmr_rxColorCodeSeen;
    report->rxTimeslot      = status->dmr_rxTimeslot;
    report->rxSyncType      = status->dmr_rxSyncType;
    report->callState       = status->dmr_callState;
    report->slotLock        = status->dmr_slotLock;
    report->markers         = status->dmr_markers;
}

static void restoreDmrReport(rtxStatus_t *status, const DmrReport *report)
{
    status->dmr_lcOk            = report->lcOk;
    status->dmr_rxSrcId         = report->rxSrcId;
    status->dmr_rxDstId         = report->rxDstId;
    status->dmr_rxFlco          = report->rxFlco;
    status->dmr_rxColorCodeSeen = report->rxColorCodeSeen;
    status->dmr_rxTimeslot      = report->rxTimeslot;
    status->dmr_rxSyncType      = report->rxSyncType;
    status->dmr_callState       = report->callState;
    status->dmr_slotLock        = report->slotLock;
    status->dmr_markers         = report->markers;
}


void rtx_init(pthread_mutex_t *m)
{
    // Initialise mutex for configuration access
    cfgMutex = m;
    newCnf   = NULL;

    /*
     * Default initialisation for rtx status
     */
    rtxStatus.opMode        = OPMODE_NONE;
    rtxStatus.bandwidth     = BW_25;
    rtxStatus.txDisable     = 0;
    rtxStatus.opStatus      = OFF;
    rtxStatus.rxFrequency   = 430000000;
    rtxStatus.txFrequency   = 430000000;
    rtxStatus.txPower       = 0.0f;
    rtxStatus.sqlLevel      = 1;
    rtxStatus.rxToneEn      = 0;
    rtxStatus.rxTone        = 0;
    rtxStatus.txToneEn      = 0;
    rtxStatus.txTone        = 0;
    rtxStatus.invertRxPhase = false;
    rtxStatus.lsfOk         = false;
    rtxStatus.M17_src[0]    = '\0';
    rtxStatus.M17_dst[0]    = '\0';
    rtxStatus.M17_link[0]   = '\0';
    rtxStatus.M17_refl[0]   = '\0';
    rtxStatus.M17_meta_text[0] = '\0';
    rtxStatus.dmr_srcId     = 0;
    rtxStatus.dmr_dstId     = 0;
    rtxStatus.dmr_callType  = 0;
    rtxStatus.dmr_rxColorCode = 1;
    rtxStatus.dmr_txColorCode = 1;
    rtxStatus.dmr_timeslot  = 1;
    rtxStatus.dmr_monitor   = 0;
    rtxStatus.dmr_polite    = 1;
    rtxStatus.dmr_lcOk      = false;
    rtxStatus.dmr_rxSrcId   = 0;
    rtxStatus.dmr_rxDstId   = 0;
    rtxStatus.dmr_rxFlco    = 0;
    rtxStatus.dmr_rxColorCodeSeen = 0;
    rtxStatus.dmr_rxTimeslot = 0;
    rtxStatus.dmr_rxSyncType = 0;
    rtxStatus.dmr_callState = DMR_CALL_IDLE;
    rtxStatus.dmr_slotLock  = 0;
    rtxStatus.dmr_markers   = 0;
    currMode = &noMode;

    /*
     * Initialise low-level platform-specific driver
     */
    radio_init(&rtxStatus);
    radio_updateConfiguration();

    /*
     * Initial value for RSSI filter
     */
    rssi         = radio_getRssi();
    reinitFilter = false;
}

void rtx_terminate()
{
    rtxStatus.opStatus = OFF;
    rtxStatus.opMode   = OPMODE_NONE;
    currMode->disable();
    radio_terminate();
}

void rtx_configure(const rtxStatus_t *cfg)
{
    /*
     * NOTE: an incoming configuration may overwrite a preceding one not yet
     * read by the radio task. This mechanism ensures that the radio driver
     * always gets the most recent configuration.
     */

    pthread_mutex_lock(cfgMutex);
    newCnf = cfg;
    pthread_mutex_unlock(cfgMutex);
}

rtxStatus_t rtx_getCurrentStatus()
{
    return rtxStatus;
}

void rtx_task()
{
    // Check if there is a pending new configuration and, in case, read it.
    bool reconfigure = false;
    if(pthread_mutex_trylock(cfgMutex) == 0)
    {
        if(newCnf != NULL)
        {
            // Copy new configuration and override opStatus flags. The DMR
            // receive report is owned by the opMode handler and survives the
            // copy as well: the UI thread never writes those fields.
            uint8_t tmp = rtxStatus.opStatus;
            DmrReport dmrReport;
            saveDmrReport(&dmrReport, &rtxStatus);
            memcpy(&rtxStatus, newCnf, sizeof(rtxStatus_t));
            rtxStatus.opStatus = tmp;
            restoreDmrReport(&rtxStatus, &dmrReport);

            reconfigure = true;
            newCnf = NULL;
        }

        pthread_mutex_unlock(cfgMutex);
    }

    if(reconfigure)
    {
        // Force TX and RX tone squelch to off for OpModes different from FM.
        if(rtxStatus.opMode != OPMODE_FM)
        {
            rtxStatus.txToneEn = 0;
            rtxStatus.rxToneEn = 0;
        }

        /*
         * Handle change of opMode:
         * - deactivate current opMode and switch operating status to "OFF";
         * - update pointer to current mode handler to the OpMode object for the
         *   selected mode;
         * - enable the new mode handler
         */
        if(currMode->getID() != rtxStatus.opMode)
        {
            // Forward opMode change also to radio driver
            radio_setOpmode(static_cast< enum opmode >(rtxStatus.opMode));

            currMode->disable();
            rtxStatus.opStatus = OFF;

            switch(rtxStatus.opMode)
            {
                case OPMODE_NONE: currMode = &noMode;  break;
                case OPMODE_FM:   currMode = &fmMode;  break;
                #ifdef CONFIG_M17
                case OPMODE_M17:  currMode = &m17Mode; break;
                #endif
                #ifdef CONFIG_DMR
                case OPMODE_DMR:  currMode = &dmrMode; break;
                #endif
                default:   currMode = &noMode;
            }

            currMode->enable();
        }

        // Tell radio driver that there was a change in its configuration.
        radio_updateConfiguration();
    }

    /*
     * RSSI update block, run only when radio is in RX mode.
     *
     * RSSI value is passed through a filter with a time constant of 60ms
     * (cut-off frequency of 15Hz) at an update rate of 33.3Hz.
     *
     * The low pass filter skips an update step if a new configuration has
     * just been applied. This is a workaround for the AT1846S returning a
     * full-scale RSSI value immediately after one of its parameters changed,
     * thus causing the squelch to open briefly.
     *
     * Also, the RSSI filter is re-initialised every time radio stage is
     * switched back from TX/OFF to RX. This provides a workaround for some
     * radios reporting a full-scale RSSI value when transmitting.
     */
    if(rtxStatus.opStatus == RX)
    {

        if(!reconfigure)
        {
            if(!reinitFilter)
            {
                /*
                 * Filter RSSI value using 15.16 fixed point math. Equivalent
                 * floating point code is: rssi = 0.74*radio_getRssi() + 0.26*rssi
                 */
                int32_t filt_rssi = radio_getRssi() * 0xBD70    // 0.74 * radio_getRssi
                                  + rssi            * 0x428F;   // 0.26 * rssi
                rssi = (filt_rssi + 32768) >> 16;               // Round to nearest
            }
            else
            {
                rssi = radio_getRssi();
                reinitFilter = false;
            }
        }
    }
    else
    {
        // Reinit required if current operating status is TX or OFF
        reinitFilter = true;
    }

    /*
     * Forward the periodic update step to the currently active opMode handler.
     * Call is placed after RSSI update to allow handler's code have a fresh
     * version of the RSSI level.
     */
    currMode->update(&rtxStatus, reconfigure);
}

rssi_t rtx_getRssi()
{
    return rssi;
}

bool rtx_rxSquelchOpen()
{
    return currMode->rxSquelchOpen();
}
