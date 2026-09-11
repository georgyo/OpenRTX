/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "core/nvmem_access.h"
#include "core/nvmem_device.h"
#include "drivers/NVM/eeep.h"
}

/*
 * In-memory emulation of a NOR flash with 4 kB erase sectors, used as backing
 * store for the EEEP driver. As on the real chip, a write can only clear bits
 * and an erase sets a whole sector to 0xFF. Every operation can be made to
 * fail to check that errors are propagated by the layers above.
 */
static constexpr uint32_t FLASH_SIZE = 0x8000;
static constexpr uint32_t SECT_SIZE = 0x1000;

static uint8_t flashMem[FLASH_SIZE];
static bool failRead = false;
static bool failWrite = false;
static bool failErase = false;

static int fakeRead(const struct nvmDevice *dev, uint32_t addr, void *data,
                    size_t len)
{
    (void)dev;

    if (failRead)
        return -EIO;

    if ((addr > FLASH_SIZE) || (len > (FLASH_SIZE - addr)))
        return -EINVAL;

    memcpy(data, &flashMem[addr], len);

    return 0;
}

static int fakeWrite(const struct nvmDevice *dev, uint32_t addr,
                     const void *data, size_t len)
{
    const uint8_t *src = static_cast<const uint8_t *>(data);

    (void)dev;

    if (failWrite)
        return -EIO;

    if ((addr > FLASH_SIZE) || (len > (FLASH_SIZE - addr)))
        return -EINVAL;

    for (size_t i = 0; i < len; i++)
        flashMem[addr + i] &= src[i];

    return 0;
}

static int fakeErase(const struct nvmDevice *dev, uint32_t addr, size_t size)
{
    (void)dev;

    if (failErase)
        return -EIO;

    if ((addr > FLASH_SIZE) || (size > (FLASH_SIZE - addr)))
        return -EINVAL;

    memset(&flashMem[addr], 0xFF, size);

    return 0;
}

static const struct nvmOps fakeOps = { fakeRead, fakeWrite, fakeErase, NULL };

static const struct nvmInfo fakeInfo = { 1, SECT_SIZE, 100000,
                                         NVM_FLASH | NVM_WRITE | NVM_ERASE };

static const struct nvmDevice fakeFlash = { NULL, &fakeOps, &fakeInfo };

/*
 * Partition layout mirroring the one of the CS7000: OEM data, EEEP storage
 * spanning two erase sectors and the remaining space up to the end.
 */
static const struct nvmPartition goodParts[] = {
    { 0x0000, 0x1000 },
    { 0x1000, 0x2000 },
    { 0x3000, 0x5000 },
};

/*
 * Deliberately malformed table: the first partition extends 4 kB beyond the
 * end of the device, the second one starts beyond the end of the device.
 */
static const struct nvmPartition badParts[] = {
    { 0x7000, 0x2000 },
    { 0xFFFFF000, 0x2000 },
};

static const struct nvmDescriptor areas[] = {
    { "Fake flash", &fakeFlash, 0, FLASH_SIZE,
      sizeof(goodParts) / sizeof(goodParts[0]), goodParts },
    { "Bad partition table", &fakeFlash, 0, FLASH_SIZE,
      sizeof(badParts) / sizeof(badParts[0]), badParts },
};

extern "C" {
const struct nvmTable nvmTab = { areas, sizeof(areas) / sizeof(areas[0]) };
}

static struct eeepData eeepPriv;
static const struct nvmDevice eeep = { &eeepPriv, &eeep_ops, &eeep_info };

static constexpr uint32_t EEEP_PART_ADDR = 0x1000;
static constexpr uint32_t EEEP_HDR_SIZE = 4;
static constexpr uint32_t EEEP_REC_SIZE = 4;
static constexpr uint32_t PAGE_ACTIVE = 0x000000FF;

static void resetFlash()
{
    memset(flashMem, 0xFF, sizeof(flashMem));
    memset(&eeepPriv, 0x00, sizeof(eeepPriv));
    failRead = false;
    failWrite = false;
    failErase = false;
}

static uint32_t readWord(uint32_t addr)
{
    uint32_t word;

    memcpy(&word, &flashMem[addr], sizeof(word));

    return word;
}

static bool flashIsFilledWith(uint32_t start, uint32_t end, uint8_t value)
{
    for (uint32_t i = start; i < end; i++) {
        if (flashMem[i] != value)
            return false;
    }

    return true;
}

TEST_CASE("nvm_getPart rejects partitions lying outside the memory area",
          "[nvm]")
{
    struct nvmPartition part;
    uint8_t buf[4];

    // Well-formed table: whole device and every partition are accepted
    REQUIRE(nvm_getPart(0, 0, &part) == 0);
    REQUIRE(part.offset == 0);
    REQUIRE(part.size == FLASH_SIZE);
    REQUIRE(nvm_getPart(0, 1, &part) == 0);
    REQUIRE(part.offset == 0x0000);
    REQUIRE(nvm_getPart(0, 3, &part) == 0);
    REQUIRE(part.offset == 0x3000);
    REQUIRE(part.size == 0x5000);
    REQUIRE(nvm_getPart(0, 4, &part) == -EINVAL);
    REQUIRE(nvm_getPart(2, 0, &part) == -EINVAL);

    // Malformed table: the whole device is still accessible, the partitions
    // extending beyond the device size are refused
    REQUIRE(nvm_getPart(1, 0, &part) == 0);
    REQUIRE(nvm_getPart(1, 1, &part) == -EINVAL);
    REQUIRE(nvm_getPart(1, 2, &part) == -EINVAL);
    REQUIRE(nvm_read(1, 1, 0, buf, sizeof(buf)) == -EINVAL);
    REQUIRE(nvm_write(1, 1, 0, buf, sizeof(buf)) == -EINVAL);
    REQUIRE(nvm_erase(1, 1, 0, SECT_SIZE) == -EINVAL);

    // Accesses are still bound-checked against the partition size
    resetFlash();
    REQUIRE(nvm_read(0, 2, 0x2000 - 2, buf, sizeof(buf)) == -EINVAL);
    REQUIRE(nvm_read(0, 2, 0x2000 - 4, buf, sizeof(buf)) == 0);
}

TEST_CASE("EEEP init formats a blank partition, zero-based partition index",
          "[nvm][eeep]")
{
    resetFlash();

    // Fill the neighbouring partitions with a pattern to detect stray writes
    memset(&flashMem[0x0000], 0xA5, 0x1000);
    memset(&flashMem[0x3000], 0x5A, 0x5000);

    REQUIRE(eeep_init(&eeep, 0, 1) == 0);

    // Index 1 is the second entry of the table, not the first one as it would
    // be with the nvm_read() numbering.
    REQUIRE(eeepPriv.nvm == &fakeFlash);
    REQUIRE(eeepPriv.part == &goodParts[1]);
    REQUIRE(eeepPriv.readAddr == EEEP_PART_ADDR + EEEP_HDR_SIZE);
    REQUIRE(eeepPriv.writeAddr == EEEP_PART_ADDR + EEEP_HDR_SIZE);

    // First page marked as active, rest of the partition erased
    REQUIRE(readWord(EEEP_PART_ADDR) == PAGE_ACTIVE);
    REQUIRE(flashIsFilledWith(EEEP_PART_ADDR + EEEP_HDR_SIZE, 0x3000, 0xFF));

    // Neighbouring partitions untouched
    REQUIRE(flashIsFilledWith(0x0000, 0x1000, 0xA5));
    REQUIRE(flashIsFilledWith(0x3000, 0x8000, 0x5A));
}

TEST_CASE("EEEP init rejects invalid device and partition indexes",
          "[nvm][eeep]")
{
    resetFlash();

    REQUIRE(eeep_init(&eeep, 2, 0) == -EINVAL);
    REQUIRE(eeep_init(&eeep, 0, 3) == -EINVAL);
    REQUIRE(eeepPriv.nvm == nullptr);
    REQUIRE(flashIsFilledWith(0x0000, FLASH_SIZE, 0xFF));
}

TEST_CASE("EEEP init propagates memory errors and disables the device",
          "[nvm][eeep]")
{
    uint8_t buf[8] = { 0 };

    SECTION("read failure while scanning for the active page")
    {
        resetFlash();
        failRead = true;

        REQUIRE(eeep_init(&eeep, 0, 1) == -EIO);
        REQUIRE(eeepPriv.nvm == nullptr);
        REQUIRE(flashIsFilledWith(0x0000, FLASH_SIZE, 0xFF));
    }

    SECTION("erase failure while formatting the partition")
    {
        resetFlash();
        failErase = true;

        REQUIRE(eeep_init(&eeep, 0, 1) == -EIO);
        REQUIRE(eeepPriv.nvm == nullptr);
    }

    SECTION("write failure while formatting the partition")
    {
        resetFlash();
        failWrite = true;

        REQUIRE(eeep_init(&eeep, 0, 1) == -EIO);
        REQUIRE(eeepPriv.nvm == nullptr);
    }

    SECTION("read failure while scanning the records of the active page")
    {
        resetFlash();
        REQUIRE(eeep_init(&eeep, 0, 1) == 0);
        REQUIRE(nvm_devWrite(&eeep, 0x0001, buf, sizeof(buf)) == 0);

        memset(&eeepPriv, 0x00, sizeof(eeepPriv));
        failRead = true;

        REQUIRE(eeep_init(&eeep, 0, 1) == -EIO);
        REQUIRE(eeepPriv.nvm == nullptr);
    }

    // After a failed initialisation the device must refuse every access
    // instead of operating on an undefined location.
    REQUIRE(nvm_devRead(&eeep, 0x0001, buf, sizeof(buf)) == -ENODEV);
    REQUIRE(nvm_devWrite(&eeep, 0x0001, buf, sizeof(buf)) == -ENODEV);
}

TEST_CASE("EEEP write and read round trip survives a re-initialisation",
          "[nvm][eeep]")
{
    const uint8_t data[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                               0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE };
    uint8_t buf[16];

    resetFlash();
    REQUIRE(eeep_init(&eeep, 0, 1) == 0);

    REQUIRE(nvm_devWrite(&eeep, 0x0002, data, sizeof(data)) == 0);
    memset(buf, 0x00, sizeof(buf));
    REQUIRE(nvm_devRead(&eeep, 0x0002, buf, sizeof(buf)) == 0);
    REQUIRE(memcmp(buf, data, sizeof(data)) == 0);

    const uint32_t firstFree = EEEP_PART_ADDR + EEEP_HDR_SIZE + EEEP_REC_SIZE
                             + sizeof(data);
    REQUIRE(eeepPriv.writeAddr == firstFree);

    // A record for an unknown address is reported as not found
    REQUIRE(nvm_devRead(&eeep, 0x0003, buf, sizeof(buf)) < 0);

    // Re-initialise on the same memory: the active page is found and the
    // write pointer is restored right after the last record.
    memset(&eeepPriv, 0x00, sizeof(eeepPriv));
    REQUIRE(eeep_init(&eeep, 0, 1) == 0);
    REQUIRE(eeepPriv.readAddr == EEEP_PART_ADDR + EEEP_HDR_SIZE);
    REQUIRE(eeepPriv.writeAddr == firstFree);

    memset(buf, 0x00, sizeof(buf));
    REQUIRE(nvm_devRead(&eeep, 0x0002, buf, sizeof(buf)) == 0);
    REQUIRE(memcmp(buf, data, sizeof(data)) == 0);

    // A newer record supersedes the previous one
    const uint8_t update[16] = { 0xAA, 0xBB, 0xCC, 0xDD };
    REQUIRE(nvm_devWrite(&eeep, 0x0002, update, sizeof(update)) == 0);
    memset(buf, 0x00, sizeof(buf));
    REQUIRE(nvm_devRead(&eeep, 0x0002, buf, sizeof(buf)) == 0);
    REQUIRE(memcmp(buf, update, sizeof(update)) == 0);
}

TEST_CASE("EEEP page swap keeps the latest value of every record",
          "[nvm][eeep]")
{
    uint8_t buf[16];

    resetFlash();
    REQUIRE(eeep_init(&eeep, 0, 1) == 0);

    // Each record takes 20 bytes: more than 200 writes fill the 4 kB page and
    // force the driver to move the live records to the second sector.
    for (uint32_t i = 0; i < 300; i++) {
        uint8_t data[16];

        memset(data, static_cast<int>(i & 0xFF), sizeof(data));
        data[0] = static_cast<uint8_t>(i % 4);
        REQUIRE(nvm_devWrite(&eeep, 0x0010 + (i % 4), data, sizeof(data)) == 0);
    }

    REQUIRE(eeepPriv.readAddr != EEEP_PART_ADDR + EEEP_HDR_SIZE);
    REQUIRE(readWord(EEEP_PART_ADDR) != PAGE_ACTIVE);

    for (uint32_t addr = 0; addr < 4; addr++) {
        // Last write for this address was iteration 296 + addr
        const uint8_t expected = static_cast<uint8_t>((296 + addr) & 0xFF);

        memset(buf, 0x00, sizeof(buf));
        REQUIRE(nvm_devRead(&eeep, 0x0010 + addr, buf, sizeof(buf)) == 0);
        REQUIRE(buf[0] == addr);
        REQUIRE(buf[1] == expected);
    }
}
