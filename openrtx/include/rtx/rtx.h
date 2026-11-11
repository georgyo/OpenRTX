/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RTX_H
#define RTX_H

#include "core/datatypes.h"
#include <stdint.h>
#include "core/cps.h"
#include "core/dmr_audio.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DMR bring-up counters and register readback, published by the DMR opMode
 * handler on every pass from the modem driver's snapshot and shown by the
 * DMR monitor screen (Settings > DMR > DMR monitor). Owned by the opMode
 * handler like the DMR receive report: rtx_task() keeps it across the copy
 * of a new configuration. Register numbers are those of the HR_C6000, see
 * interfaces/dmr_baseband.h.
 */
typedef struct
{
    uint32_t tsCount;      /**< TIME_SLOT_INTER interrupts                  */
    uint32_t sysCount;     /**< SYS_INTER interrupts                        */
    uint32_t rftxCount;    /**< RF_TX_INTER interrupts                      */
    uint32_t lineCount[4]; /**< EXTI pulses on PC0..PC3 during bring-up     */
    uint32_t lcSrc;        /**< Source address decoded from lc[]            */
    uint32_t lcDst;        /**< Destination address decoded from lc[]       */
    uint16_t rssi;         /**< Registers 0x43:0x44                         */
    uint8_t  irq82;        /**< 0x82 flags of the last SYS event            */
    uint8_t  sub84;        /**< 0x84 TX start sub-status of the last SYS    */
    uint8_t  sub86;        /**< 0x86 TX end sub-status of the last SYS      */
    uint8_t  r51;          /**< 0x51 data type/sequence, CRC, sync class    */
    uint8_t  r52;          /**< 0x52 colour code, CACH AT/TC, LCSS          */
    uint8_t  r5f;          /**< 0x5F sync type in [1:0]                     */
    uint8_t  r42;          /**< 0x42 slot status of the last TS event       */
    uint8_t  fskErr;       /**< Register 0x0F                               */
    uint8_t  timecode;     /**< Local timecode of the current slot          */
    uint8_t  lc[12];       /**< LC RAM of the last control frame            */
    uint8_t  lcFlco;       /**< FLCO decoded from lc[]                      */
    uint8_t  lcFid;        /**< FID decoded from lc[]                       */
    uint8_t  lcValid;      /**< bit0: chip CRC good (0x51 bit2 clear),
                                bit1: lc[] parsed as a standard Full LC     */
    uint32_t txBursts;     /**< Own voice bursts sent since enable()        */
    uint32_t txSilent;     /**< Of which carrying the AMBE silence pattern
                                because the audio layer had no payload      */
    uint16_t txUnderruns;  /**< Silent bursts while the encoder was running:
                                the microphone stream fell behind          */
    uint16_t rfKeyed;      /**< Times the RF stage was keyed for TX         */
    uint32_t shortLc;      /**< Last Short LC with a good CRC, 28 bits      */
    uint16_t shortLcOk;    /**< Short LCs received with a good CRC          */
    uint16_t shortLcBad;   /**< Short LCs dropped for a bad CRC             */
}
dmrMonitor_t;

typedef struct
{
    uint8_t opMode;         /**< Operating mode (FM, DMR, ...) */

    uint8_t bandwidth : 2,  /**< Channel bandwidth             */
            txDisable : 1,  /**< Disable TX operation          */
            scan      : 1,  /**< Scan enabled                  */
            opStatus  : 2,  /**< Operating status (OFF, ...)   */
            _padding  : 2;  /**< Padding to 8 bits             */

    freq_t rxFrequency;     /**< RX frequency, in Hz           */
    freq_t txFrequency;     /**< TX frequency, in Hz           */

    uint32_t txPower;       /**< TX power, in mW               */
    uint8_t  sqlLevel;      /**< Squelch opening level         */

    uint16_t rxToneEn : 1,  /**< RX CTC/DCS tone enable        */
             rxTone   : 15; /**< RX CTC/DCS tone               */

    uint16_t txToneEn : 1,  /**< TX CTC/DCS tone enable        */
             txTone   : 15; /**< TX CTC/DCS tone               */

    bool     toneEn;

    uint8_t  can      : 4,  /**< M17 Channel Access Number     */
             canRxEn  : 1,  /**< M17 Check CAN on RX           */
             _unused  : 3;

    char     source_address[10];       /**< M17 call source address    */
    char     destination_address[10];  /**< M17 call routing address   */
    bool     invertRxPhase;            /**< M17 RX phase inversion     */
    bool     lsfOk;                    /**  M17 LSF is valid           */
    char     M17_dst[10];              /**  M17 LSF destination        */
    char     M17_src[10];              /**  M17 LSF source             */
    char     M17_link[10];             /**  M17 LSF traffic originator */
    char     M17_refl[10];             /**  M17 LSF reflector module   */
    char     M17_meta_text[53];        /**< M17 Meta Text              */

    /* DMR configuration, pushed by the UI thread */
    uint32_t dmr_srcId;        /**< DMR own ID, settings.dmr_id            */
    uint32_t dmr_dstId;        /**< DMR talkgroup or target ID             */
    uint8_t  dmr_callType;     /**< DMR call type, enum dmrContactType_t   */
    uint8_t  dmr_rxColorCode;  /**< DMR RX colour code, 0-15               */
    uint8_t  dmr_txColorCode;  /**< DMR TX colour code, 0-15               */
    uint8_t  dmr_timeslot;     /**< DMR timeslot, 1 or 2                   */
    uint8_t  dmr_monitor;      /**< DMR monitor: 0 CC+TS+address filter,
                                    1 own CC any TG, 2 any CC/TS           */
    uint8_t  dmr_polite;       /**< DMR channel access: 1 polite to own CC,
                                    0 impolite                             */
    uint8_t  dmr_modeReg;      /**< DMR modem mode register 0x10 override
                                    from the monitor screen, 0 = default   */
    uint8_t  dmr_lcssMode;     /**< DMR LCSS convention of the voice bursts
                                    (register 0x50): 0 chip-filled, 1 the
                                    manual's per-burst sequence, from the
                                    monitor screen                         */
    uint8_t  dmr_rfAdvance;    /**< DMR RF_TX_INTER advance register 0x12
                                    override from the monitor screen, 0 =
                                    driver default (bit6, the two-point
                                    test tone, is never carried here)      */

    /* DMR receive report, written by the DMR opMode handler */
    bool     dmr_lcOk;         /**< DMR Full LC with valid CRC is held     */
    uint32_t dmr_rxSrcId;      /**< DMR source ID of the received call     */
    uint32_t dmr_rxDstId;      /**< DMR destination ID of the received call*/
    uint8_t  dmr_rxFlco;       /**< DMR FLCO of the received LC: 0 group,
                                    3 unit-to-unit, 4-7 talker alias, 8 GPS*/
    uint8_t  dmr_rxColorCodeSeen; /**< DMR colour code of the received burst*/
    uint8_t  dmr_rxTimeslot;   /**< DMR timeslot of the received call      */
    uint8_t  dmr_rxSyncType;   /**< DMR sync seen: 0 MS/DMO, 1 BS/RMO      */
    uint8_t  dmr_callState;    /**< DMR call state, enum dmrCallState      */
    uint8_t  dmr_slotLock;     /**< DMR timing: 0 none, 1 synced,
                                    2 timecode locked                      */
    uint8_t  dmr_markers;      /**< DMR top-bar markers, enum dmrMarker    */
    uint8_t  dmr_tsBusy;       /**< DMR repeater slot activity from the
                                    Act_Updt Short LC: bit0 TS1 busy, bit1
                                    TS2 busy; cleared 2 s after the last
                                    Short LC of any kind (a Null message
                                    keeps them alive), on a retune and on
                                    a modem reprogramming                  */
    dmrMonitor_t dmr_mon;      /**< DMR bring-up counters, monitor screen  */
    struct dmrAudioStats dmr_audio; /**< DMR audio layer counters          */
}
rtxStatus_t;

/**
 * \enum bandwidth Enumeration type defining the current rtx bandwidth.
 */
enum bandwidth
{
    BW_12_5 = 0,    /**< 12.5kHz bandwidth */
    BW_25   = 1     /**< 25kHz bandwidth   */
};

/**
 * \enum opmode Enumeration type defining the current rtx operating mode.
 */
enum opmode
{
    OPMODE_NONE = 0,        /**< No opMode selected */
    OPMODE_FM   = 1,        /**< Analog FM          */
    OPMODE_DMR  = 2,        /**< DMR                */
    OPMODE_M17  = 3         /**< M17                */
};

/**
 * \enum dmrCallState Enumeration type defining the state of a DMR call, as
 * reported by the DMR opMode handler in rtxStatus_t::dmr_callState.
 */
enum dmrCallState
{
    DMR_CALL_IDLE      = 0,     /**< No call in progress                  */
    DMR_CALL_RX        = 1,     /**< Receiving a call                     */
    DMR_CALL_RX_HANG   = 2,     /**< Call ended, hang time running        */
    DMR_CALL_TX        = 3,     /**< Transmitting                         */
    DMR_CALL_TX_WAKEUP = 4      /**< Waking up a repeater before TX       */
};

/**
 * \enum dmrMarker Bit mask of the markers the DMR opMode handler raises for
 * the UI top bar in rtxStatus_t::dmr_markers. All but DMR_MARK_NOT_SUPPORTED
 * are cleared at the next PTT press or a few seconds after being raised; that
 * one stays as long as DMR mode is selected on a radio without a DMR modem.
 */
enum dmrMarker
{
    DMR_MARK_NO_ID         = 0x01,  /**< PTT refused: no DMR ID configured  */
    DMR_MARK_BUSY          = 0x02,  /**< TX rejected: channel busy          */
    DMR_MARK_WAKEUP_FAILED = 0x04,  /**< Repeater did not answer the wakeup */
    DMR_MARK_NOT_SUPPORTED = 0x08,  /**< No DMR modem on this radio         */
    DMR_MARK_RF_FAILED     = 0x10   /**< RF stage refused to key (PLL lock) */
};

/**
 * \enum opstatus Enumeration type defining the current rtx operating status.
 */
enum opstatus
{
    OFF = 0,        /**< OFF          */
    RX  = 1,        /**< Receiving    */
    TX  = 2         /**< Transmitting */
};


/**
 * Initialise rtx stage.
 * @param m: pointer to the mutex protecting the shared configuration data
 * structure.
 */
void rtx_init(pthread_mutex_t *m);

/**
 * Shut down rtx stage
 */
void rtx_terminate();

/**
 * Post a new RTX configuration on the internal message queue. Data structure
 * \b must be protected by the same mutex whose pointer has been passed as a
 * parameter to rtx_init(). This driver only copies its content into the internal
 * data structure, eventual garbage collection has to be performed by caller.
 * @param cfg: pointer to a structure containing the new RTX configuration.
 */
void rtx_configure(const rtxStatus_t *cfg);

/**
 * Obtain a copy of the RTX driver's internal status data structure.
 * @return copy of the RTX driver's internal status data structure.
 */
rtxStatus_t rtx_getCurrentStatus();

/**
 * High-level code is in charge of calling this function periodically, since it
 * contains all the RTX management functionalities.
 */
void rtx_task();

/**
 * Get current RSSI in dBm.
 * @return RSSI value in dBm.
 */
rssi_t rtx_getRssi();

/**
 * Get current status of the RX squelch. This function is thread-safe and can
 * be called also from threads other than the one running the RTX task.
 * @return true if RX squelch is open.
 */
bool rtx_rxSquelchOpen();

/**
 * Get the DMR top-bar markers raised by the DMR operating mode, without
 * copying the whole status. Reading one byte is safe from any thread.
 * @return dmr_markers of the current status, 0 when not in DMR mode.
 */
uint8_t rtx_getDmrMarkers();

#ifdef __cplusplus
}
#endif

#endif /* RTX_H */
