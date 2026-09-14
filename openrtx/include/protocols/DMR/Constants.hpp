/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_CONSTANTS_H
#define DMR_CONSTANTS_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstddef>
#include <cstdint>

/*
 * DMR air interface constants, written from the ETSI text:
 *
 *  - ETSI TS 102 361-1 V2.5.1 (2017-10), "DMR Air Interface (AI) protocol"
 *  - ETSI TS 102 361-2 V2.4.1 (2017-10), "DMR voice and generic services and
 *    facilities"
 *
 * Every constant carries the clause it was taken from. Nothing in this file
 * touches hardware: the HR_C6000 baseband does the framing and FEC on its own,
 * these values exist so that the protocol layer, the state machine and the
 * host tests speak the same language as the specification.
 */

namespace DMR
{

/*
 * TS 102 361-1 §3.1 and §4.2.2: a timeslot (burst) lasts 30 ms, of which
 * 27.5 ms carry the 264 bits of payload; a TDMA frame is two timeslots,
 * 60 ms, so one logical channel gets a burst every 60 ms; a voice superframe
 * is six bursts of one channel, "A" to "F", 360 ms.
 */
static constexpr uint32_t BURST_MS = 30;
static constexpr uint32_t BURST_BITS = 264;
static constexpr uint32_t TDMA_FRAME_MS = 2 * BURST_MS;
static constexpr uint32_t SUPERFRAME_BURST = 6;
static constexpr uint32_t SUPERFRAME_MS = SUPERFRAME_BURST * TDMA_FRAME_MS;

/*
 * Synchronisation patterns, TS 102 361-1 §9.1.1 Table 9.2. The 48 bits are
 * held in the low 48 bits of a uint64_t, first transmitted bit at bit 47.
 * Voice and data patterns of the same source are the symbol-wise complement
 * of each other (note to Table 9.2), i.e. they differ by 0xAAAAAAAAAAAA.
 */
static constexpr size_t SYNC_BITS = 48;
static constexpr uint64_t SYNC_MASK = 0xFFFFFFFFFFFFULL;
static constexpr uint64_t SYNC_BS_VOICE = 0x755FD7DF75F7ULL;
static constexpr uint64_t SYNC_BS_DATA = 0xDFF57D75DF5DULL;
static constexpr uint64_t SYNC_MS_VOICE = 0x7F7D5DD57DFDULL;
static constexpr uint64_t SYNC_MS_DATA = 0xD5D7F77FD757ULL;
static constexpr uint64_t SYNC_MS_RC = 0x77D55F7DFD77ULL;
static constexpr uint64_t SYNC_DMO_TS1_VOICE = 0x5D577F7757FFULL;
static constexpr uint64_t SYNC_DMO_TS1_DATA = 0xF7FDD5DDFD55ULL;
static constexpr uint64_t SYNC_DMO_TS2_VOICE = 0x7DFFD5F55D5FULL;
static constexpr uint64_t SYNC_DMO_TS2_DATA = 0xD7557F5FF7F5ULL;
static constexpr uint64_t SYNC_RESERVED = 0xDD7FF5D757DDULL;

/*
 * Data Type information element, TS 102 361-1 §9.3.6 Table 9.22. Carried in
 * the Slot Type field of every general data burst.
 */
enum DataType : uint8_t {
    DT_PI_HEADER = 0,
    DT_VOICE_LC_HEADER = 1,
    DT_TERMINATOR_LC = 2,
    DT_CSBK = 3,
    DT_MBC_HEADER = 4,
    DT_MBC_CONTINUATION = 5,
    DT_DATA_HEADER = 6,
    DT_RATE_1_2_DATA = 7,
    DT_RATE_3_4_DATA = 8,
    DT_IDLE = 9,
    DT_RATE_1_DATA = 10,
    DT_USBD = 11, /* Unified Single Block Data */
};

/*
 * Full Link Control Opcodes, TS 102 361-2 §7.1.1 and Annex B.1 Table B.1.
 */
enum Flco : uint8_t {
    FLCO_GRP_V_CH_USR = 0,      /* Group Voice Channel User,     §7.1.1.1 */
    FLCO_UU_V_CH_USR = 3,       /* Unit to Unit Voice Ch. User,  §7.1.1.2 */
    FLCO_TALKER_ALIAS_HDR = 4,  /* Talker Alias header,          §7.1.1.4 */
    FLCO_TALKER_ALIAS_BLK1 = 5, /* Talker Alias block 1,         §7.1.1.5 */
    FLCO_TALKER_ALIAS_BLK2 = 6, /* Talker Alias block 2,         §7.1.1.5 */
    FLCO_TALKER_ALIAS_BLK3 = 7, /* Talker Alias block 3,         §7.1.1.5 */
    FLCO_GPS_INFO = 8,          /* GPS Info,                     §7.1.1.3 */
};

/*
 * Control Signalling BlocK Opcodes, TS 102 361-2 Annex B.2 Table B.2.
 */
enum Csbko : uint8_t {
    CSBKO_UU_V_REQ = 0x04,   /* Unit to Unit Voice Service Request      */
    CSBKO_UU_ANS_RSP = 0x05, /* Unit to Unit Voice Service Answer Resp. */
    CSBKO_CT_CSBK = 0x07,    /* Channel Timing CSBK                     */
    CSBKO_NACK_RSP = 0x26,   /* Negative Acknowledgement Response       */
    CSBKO_BS_DWN_ACT = 0x38, /* BS Outbound Activation, 111000b         */
    CSBKO_PRE_CSBK = 0x3D,   /* Preamble CSBK                           */
};

/*
 * LC Start/Stop information element, TS 102 361-1 §9.3.3 Table 9.20.
 */
enum Lcss : uint8_t {
    LCSS_SINGLE_OR_CSBK_FIRST = 0, /* Single fragment LC / first CSBK */
    LCSS_FIRST = 1,                /* First fragment of LC            */
    LCSS_LAST = 2,                 /* Last fragment of LC or CSBK     */
    LCSS_CONTINUATION = 3,         /* Continuation fragment           */
};

/*
 * Feature set ID, TS 102 361-1 §9.3.5 Table 9.21: 0x00 is the standardised
 * feature set (SFID), 0x01-0x03 are reserved for future standardisation and
 * everything else is a manufacturer's feature set (MFID).
 */
static constexpr uint8_t FID_STANDARD = 0x00;
static constexpr uint8_t FID_RESERVED_MAX = 0x03;

/*
 * Service Options information element, TS 102 361-2 §7.2.1 Table 7.11, bit 7
 * is the first transmitted bit.
 */
enum ServiceOptions : uint8_t {
    SO_EMERGENCY = 0x80,     /* Emergency service                       */
    SO_PRIVACY = 0x40,       /* Privacy, not defined by TS 102 361-2    */
    SO_BROADCAST = 0x08,     /* Broadcast (one-way) group call          */
    SO_OVCM = 0x04,          /* Open Voice Call Mode                    */
    SO_PRIORITY_MASK = 0x03, /* Priority level, 3 is the highest        */
};

/*
 * Addressing, TS 102 361-1 Annex A: addresses are 24 bits wide, 0xFFFFFF is
 * the default "All unit Id" / "All talkgroup Id" addressing everybody.
 */
static constexpr uint32_t ADDRESS_MASK = 0xFFFFFF;
static constexpr uint32_t ADDRESS_ALL = 0xFFFFFF;

/*
 * Data Type CRC masks, TS 102 361-1 Annex B.3.12 Table B.21. The mask is
 * XOR-ed with the CRC after the CRC computation, before the FEC encoder.
 */
static constexpr uint32_t CRC_MASK_PI_HEADER = 0x6969;
static constexpr uint32_t CRC_MASK_VOICE_LC_HEADER = 0x969696;
static constexpr uint32_t CRC_MASK_TERMINATOR_LC = 0x999999;
static constexpr uint32_t CRC_MASK_CSBK = 0xA5A5;
static constexpr uint32_t CRC_MASK_MBC_HEADER = 0xAAAA;
static constexpr uint32_t CRC_MASK_DATA_HEADER = 0xCCCC;
static constexpr uint32_t CRC_MASK_USBD = 0x3333;
static constexpr uint32_t CRC_MASK_RATE_1_2_DATA = 0x0F0;
static constexpr uint32_t CRC_MASK_RATE_3_4_DATA = 0x1FF;
static constexpr uint32_t CRC_MASK_RATE_1_DATA = 0x10F;
static constexpr uint32_t CRC_MASK_RC = 0x7A;

/*
 * Sizes of the link control PDUs.
 */
static constexpr size_t FULL_LC_BYTES = 9;    /* TS 102 361-1 §7.1 Fig. 7.1 */
static constexpr size_t CSBK_BYTES = 12;      /* TS 102 361-1 §7.2 Fig. 7.8 */
static constexpr size_t CSBK_DATA_BYTES = 10; /* CSBK without the CRC      */
static constexpr size_t CHIP_LC_BYTES = 12;   /* 9 LC + 3 RS(12,9) parity  */

/*
 * Timers and constants.
 *
 *  - T_CallHt: call hangtime, default 3 s (TS 102 361-1 Annex F.1).
 *  - T_TO: transmit timeout, 180 s for Tier I, designer's choice between 0
 *    (disabled) and 180 s for Tier II/III (TS 102 361-2 §6.1, Annex A.1).
 *  - T_SyncWu: time spent looking for a sync after a wakeup, maximum 360 ms
 *    (TS 102 361-1 Annex F.1).
 *  - N_Wakeup: wakeup attempts before giving up, suggested 2
 *    (TS 102 361-1 Annex F.2).
 */
static constexpr uint32_t T_CALLHT_MS = 3000;
static constexpr uint32_t T_TO_S = 180;
static constexpr uint32_t T_SYNCWU_MS = 360;
static constexpr uint8_t N_WAKEUP = 2;

/*
 * Voice payload of a burst: 216 bits, 27 bytes, holding three AMBE+2 frames
 * (TS 102 361-1 §6.1, Figure 6.2).
 */
static constexpr size_t VOICE_PAYLOAD_BYTES = 27;

/*
 * AMBE+2 silence pattern: one burst of already encoded "silence" frames,
 * used to fill a voice burst when no vocoder output is available. The values
 * are the only thing in this tree taken from a third party:
 *
 *   MMDVMHost, DMRDefines.h ("DMR_SILENCE_DATA"),
 *   Copyright (C) 2015,2016 by Jonathan Naylor G4KLX,
 *   SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A 27-byte constant is not codec code: no AMBE encoder or decoder lives in
 * this tree.
 */
static constexpr uint8_t AMBE_SILENCE[VOICE_PAYLOAD_BYTES] = {
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

} /* namespace DMR */

#endif /* DMR_CONSTANTS_H */
