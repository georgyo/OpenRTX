/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/Silence.h"

/*
 * AMBE+2 silence pattern: one burst of already encoded "silence" frames,
 * used to fill a voice burst when no vocoder output is available. The values
 * are the only thing in the DMR code of this tree taken from a third party:
 *
 *   MMDVMHost, DMRDefines.h ("DMR_SILENCE_DATA"),
 *   Copyright (C) 2015,2016 by Jonathan Naylor G4KLX,
 *   SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A 27-byte constant is not codec code: no AMBE encoder or decoder lives in
 * this tree. The same nine bytes repeat three times, one per 20 ms frame.
 */
const uint8_t dmr_ambeSilence[DMR_AMBE_SILENCE_BYTES] = {
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};
