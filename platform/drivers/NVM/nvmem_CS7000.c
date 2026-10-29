/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/nvmem.h"
#include "interfaces/delays.h"
#include "calibration/calibInfo_CS7000.h"
#include "core/nvmem_access.h"
#include "drivers/SPI/spi_bitbang.h"
#include "drivers/SPI/spi_stm32.h"
#include <string.h>
#include "wchar.h"
#include "core/utils.h"
#include "core/crc.h"
#include "drivers/NVM/W25Qx.h"
#include "drivers/NVM/eeep.h"

static const struct W25QxCfg cfg =
{
    .spi = (const struct spiDevice *) &flash_spi,
    .cs  = { FLASH_CS }
};

W25Qx_DEVICE_DEFINE(eflash, cfg)
EEEP_DEVICE_DEFINE(eeep)

/*
 * External flash layout.
 *
 * nvm_read(), nvm_write() and nvm_erase() only bound-check an access against
 * the size of the partition it targets, so a partition extending beyond the
 * end of the flash would not be caught at runtime: the W25Q256 ignores the
 * address bits above A24 and the access would silently wrap around into the
 * OEM calibration area at the beginning of the chip. The static assertions
 * below guarantee that the partitions are in order, non-overlapping and
 * entirely contained in the flash.
 */
#ifdef PLATFORM_CS7000P
#define EFLASH_SIZE     0x2000000   // 32 MB, 256 Mbit
#define EEEP_PART_ADDR  0x1000000   // Second partition, EEEP storage
#define FREE_PART_ADDR  0x100C000   // Third partition, available memory
#else
#define EFLASH_SIZE     0x1000000   // 16 MB, 128 Mbit
#define EEEP_PART_ADDR  0x8000      // Second partition, EEEP storage
#define FREE_PART_ADDR  0xC000      // Third partition, available memory
#endif
#define CAL_PART_ADDR   0x0000      // First partition, calibration and OEM data
#define CAL_PART_SIZE   0x8000      // 32 kB
#define EEEP_PART_SIZE  0x4000      // 16 kB
#define FREE_PART_SIZE  0xFF4000    // Up to the end of the flash

_Static_assert((CAL_PART_ADDR + CAL_PART_SIZE) <= EEEP_PART_ADDR,
               "Calibration partition overlaps the EEEP partition");
_Static_assert((EEEP_PART_ADDR + EEEP_PART_SIZE) <= FREE_PART_ADDR,
               "EEEP partition overlaps the third partition");
_Static_assert((FREE_PART_ADDR + FREE_PART_SIZE) <= EFLASH_SIZE,
               "Third partition extends beyond the end of the external flash");

const struct nvmPartition memPartitions[] =
{
    {
        .offset = CAL_PART_ADDR,    // First partition, calibration and OEM data
        .size   = CAL_PART_SIZE
    },
    {
        .offset = EEEP_PART_ADDR,   // Second partition EEEP storage
        .size   = EEEP_PART_SIZE
    },
    {
        .offset = FREE_PART_ADDR,   // Third partition, available memory
        .size   = FREE_PART_SIZE
    }
};

static const struct nvmDescriptor extMem[] =
{
    {
        .name       = "External flash",
        .dev        = &eflash,
        .baseAddr   = 0x00000000,
        .size       = EFLASH_SIZE,
        .nbPart     = sizeof(memPartitions)/sizeof(struct nvmPartition),
        .partitions = memPartitions
    },
    {
        .name       = "Virtual EEPROM",
        .dev        = &eeep,
        .baseAddr   = 0x00000000,
        .size       = 0xFFFFFFFF,       // Dummy value, this device has no physical size
        .nbPart     = 0,
        .partitions = NULL
    }
};

const struct nvmTable nvmTab = {
    .areas = extMem,
    .nbAreas = ARRAY_SIZE(extMem),
};

static uint16_t settingsCrc;
static uint16_t vfoCrc;

/*
 * The settings and the VFO are stored as EEEP records, and eeep_read() and
 * eeep_write() refuse a length of 255 bytes or more: growing settings_t past
 * that would silently stop the settings from being saved (settings_t is
 * 108 bytes with the FRS fields).
 */
_Static_assert(sizeof(settings_t) < 255, "settings_t must fit an EEEP record");
_Static_assert(sizeof(channel_t) < 255, "channel_t must fit an EEEP record");

void nvm_init()
{
#ifdef PLATFORM_CS7000P
    gpio_setMode(FLASH_CLK, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(FLASH_SDI, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(FLASH_SDO, ALTERNATE | ALTERNATE_FUNC(5));
    spiStm32_init(&flash_spi, 25000000, 0);
#else
    spiBitbang_init(&flash_spi);
#endif
    W25Qx_init(&eflash);

    // NOTE: eeep_init() takes a zero-based index into the partition table,
    // unlike nvm_read() and friends where partition 0 is the whole device and
    // partition N is memPartitions[N - 1]: index 1 here is the EEEP partition,
    // while the calibration data below is read from partition 1 of the nvm
    // API, that is memPartitions[0].
    // On failure the EEEP device refuses all accesses, so settings and VFO
    // fall back to their defaults instead of touching random flash areas.
    eeep_init(&eeep, 0, 1);
}

void nvm_terminate()
{
    eeep_terminate(&eeep);
    W25Qx_terminate(&eflash);
#ifdef PLATFORM_CS7000P
    spiStm32_terminate(&flash_spi);
#else
    spiBitbang_terminate(&flash_spi);
#endif
}

void nvm_readCalibData(void *buf)
{
    struct CS7000Calib *calData = (struct CS7000Calib *) buf;

    nvm_read(0, 1, 0x1000, &(calData->txCalFreq),      sizeof(calData->txCalFreq));
    nvm_read(0, 1, 0x1020, &(calData->rxCalFreq),      sizeof(calData->rxCalFreq));
    nvm_read(0, 1, 0x1044, &(calData->rxSensitivity),  sizeof(calData->rxSensitivity));
    nvm_read(0, 1, 0x106C, &(calData->txHighPwr),      sizeof(calData->txHighPwr));
    nvm_read(0, 1, 0x1074, &(calData->txMiddlePwr),    sizeof(calData->txMiddlePwr));
    nvm_read(0, 1, 0x10C4, &(calData->txDigitalPathQ), sizeof(calData->txDigitalPathQ));
    nvm_read(0, 1, 0x10CC, &(calData->txAnalogPathI),  sizeof(calData->txAnalogPathI));
    nvm_read(0, 1, 0x10DC, &(calData->errorRate),      sizeof(calData->errorRate));

    for(int i = 0; i < 8; i++)
    {
        calData->txCalFreq[i] = __builtin_bswap32(calData->txCalFreq[i]);
        calData->rxCalFreq[i] = __builtin_bswap32(calData->rxCalFreq[i]);
    }
}

void nvm_readHwInfo(hwInfo_t *info)
{
    (void) info;
}

int nvm_readVfoChannelData(channel_t *channel)
{
    memset(channel, 0x00, sizeof(channel_t));
    int ret = nvm_read(1, 0, 0x0001, channel, sizeof(channel_t));
    if(ret < 0)
        return -1;

    vfoCrc = crc_ccitt(channel, sizeof(channel_t));

    return 0;
}

int nvm_readSettings(settings_t *settings)
{
    memset(settings, 0x00, sizeof(settings_t));
    int ret = nvm_read(1, 0, 0x0002, settings, sizeof(settings_t));
    if(ret < 0)
        return -1;

    settingsCrc = crc_ccitt(settings, sizeof(settings_t));

    return 0;
}

int nvm_writeSettings(const settings_t *settings)
{
    (void) settings;

    return -1;
}

int nvm_writeSettingsAndVfo(const settings_t *settings, const channel_t *vfo)
{
    uint16_t crc = crc_ccitt(vfo, sizeof(channel_t));
    if(crc != vfoCrc)
        nvm_write(1, 0, 0x0001, vfo, sizeof(channel_t));

    crc = crc_ccitt(settings, sizeof(settings_t));
    if(crc != settingsCrc)
        nvm_write(1, 0, 0x0002, settings, sizeof(settings_t));

    return 0;
}
