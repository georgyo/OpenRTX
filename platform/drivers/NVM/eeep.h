/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EEEP_H
#define EEEP_H

#include "interfaces/nvmem.h"

/**
 * Driver for Emulated EEPROM, providing a means to store recurrent data without
 * stressing too much the same sector of a NOR or NAND flash memory.
 */

/**
 * Device driver and information block for EEEPROM memory.
 */
extern const struct nvmOps  eeep_ops;
extern const struct nvmInfo eeep_info;

/**
 * Driver private data.
 */
struct eeepData
{
    const struct nvmDevice    *nvm;         ///< Underlying NVM device
    const struct nvmPartition *part;        ///< Memory partition used for EEPROM emulation
    uint32_t                  readAddr;     ///< Physical start address for EEEPROM reads
    uint32_t                  writeAddr;    ///< Physical start address for EEEPROM writes
};

/**
 * Instantiate a EEEPROM NVM device.
 *
 * @param name: device name.
 * @param path: full path of the file used for data storage.
 * @param size: size of the storage file, in bytes.
 */
#define EEEP_DEVICE_DEFINE(name)        \
static struct eeepData eeepData_##name; \
struct nvmDevice name =                 \
{                                       \
    .priv = &eeepData_##name,           \
    .ops  = &eeep_ops,                  \
    .info = &eeep_info,                 \
};

/**
 * Initialize an EEEP driver instance.
 *
 * NOTE: the partition index is zero-based, that is partition 0 is the first
 * entry of the partition table of the underlying NVM device. This differs from
 * the nvm_read()/nvm_write()/nvm_erase() convention, where partition 0 is the
 * whole device and partition N is the (N - 1)-th entry of the table.
 *
 * When initialisation fails the device rejects all the read and write requests
 * with -ENODEV.
 *
 * @param dev: pointer to device descriptor.
 * @param nvm: index of the underlying NVM device used for data storage.
 * @param part: zero-based index of the NVM partition used for data storage.
 * @return zero on success, a negative error code otherwise.
 */
int eeep_init(const struct nvmDevice *dev, const uint32_t nvm,
              const uint32_t part);

/**
 * Shut down an EEEP driver instance.
 *
 * @param dev: pointer to device descriptor.
 * @return zero on success, a negative error code otherwise.
 */
int eeep_terminate(const struct nvmDevice *dev);

#endif /* EEEP_H */
