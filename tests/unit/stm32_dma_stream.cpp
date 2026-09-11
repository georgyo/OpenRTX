/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests for the STM32 DMA stream handler, compiled against the fake
 * register and kernel definitions in tests/unit/fake_stm32. The header under
 * test is selected through the DMA_STREAM_HEADER macro so that the same tests
 * run for both the STM32F4 and the STM32H7 versions of the driver.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <new>

#include DMA_STREAM_HEADER

// Definitions of the fake peripherals declared by the fake stm32h7xx.h
DMA_TypeDef fakeDma[2];
DMA_Stream_TypeDef fakeDmaStreams[16];
RCC_TypeDef fakeRcc;

/**
 * Fake DMA stream register block with a zero-initialised register set.
 */
static DMA_Stream_TypeDef makeStream()
{
    DMA_Stream_TypeDef s = { { "CR", nullptr, 0 },   { "NDTR", nullptr, 0 },
                             { "PAR", nullptr, 0 },  { "M0AR", nullptr, 0 },
                             { "M1AR", nullptr, 0 }, { "FCR", nullptr, 0 } };
    return s;
}

/**
 * Storage for a StreamHandler pre-filled with garbage, to check that the
 * constructor initialises every member the handler relies on.
 */
struct GarbageStorage {
    alignas(StreamHandler) unsigned char raw[sizeof(StreamHandler)];

    GarbageStorage()
    {
        memset(raw, 0xFF, sizeof(raw));
    }

    StreamHandler *construct(DMA_Stream_TypeDef *s)
    {
        return new (raw) StreamHandler(s, DMA2_Stream0_IRQn);
    }
};

TEST_CASE("StreamHandler starts idle even from garbage storage", "[stm32][dma]")
{
    DMA_Stream_TypeDef stream = makeStream();
    stream.CR.value = DMA_SxCR_TCIE | DMA_SxCR_TEIE
                    | (1U << DMA_SxCR_MSIZE_Pos);

    GarbageStorage storage;
    StreamHandler *hdl = storage.construct(&stream);

    REQUIRE(hdl->running() == false);
    REQUIRE(hdl->idleBuf() == static_cast<void *>(nullptr));

    // Spurious interrupt with nobody waiting and no callback: must not touch
    // an uninitialised thread pointer or callback.
    hdl->IRQhandler(DMA_LISR_TCIF0);
    REQUIRE(hdl->running() == false);

    // Circular transfer: a half-transfer interrupt must not stop the stream,
    // which would happen if the stop request flag started as garbage.
    uint16_t buffer[8];
    hdl->start(&stream.NDTR, buffer, 8, true);
    REQUIRE(hdl->running() == true);
    hdl->IRQhandler(DMA_LISR_HTIF0);
    REQUIRE(hdl->running() == true);

    hdl->~StreamHandler();
}

TEST_CASE("Circular stream keeps running until a stop is requested",
          "[stm32][dma]")
{
    DMA_Stream_TypeDef stream = makeStream();
    stream.CR.value = DMA_SxCR_TCIE | DMA_SxCR_TEIE
                    | (1U << DMA_SxCR_MSIZE_Pos);

    int endCalls = 0;
    uint16_t buffer[8];
    {
        StreamHandler hdl(&stream, DMA2_Stream0_IRQn);
        hdl.setEndTransferCallback([&endCalls]() { endCalls += 1; });

        hdl.start(&stream.NDTR, buffer, 8, true);
        REQUIRE((stream.CR & (DMA_SxCR_EN | DMA_SxCR_CIRC | DMA_SxCR_HTIE))
                == (DMA_SxCR_EN | DMA_SxCR_CIRC | DMA_SxCR_HTIE));

        hdl.IRQhandler(DMA_LISR_HTIF0);
        hdl.IRQhandler(DMA_LISR_TCIF0);
        REQUIRE(hdl.running() == true);
        REQUIRE(endCalls == 0);

        hdl.stop();
        hdl.IRQhandler(DMA_LISR_TCIF0);
        REQUIRE(hdl.running() == false);
        REQUIRE(endCalls == 1);
    }

    // Destructor shuts the stream down
    REQUIRE(stream.CR == 0U);
}

TEST_CASE("Linear stream ends at transfer complete", "[stm32][dma]")
{
    DMA_Stream_TypeDef stream = makeStream();
    stream.CR.value = DMA_SxCR_TCIE | DMA_SxCR_TEIE
                    | (1U << DMA_SxCR_MSIZE_Pos);

    int endCalls = 0;
    uint16_t buffer[8];
    StreamHandler hdl(&stream, DMA2_Stream0_IRQn);
    hdl.setEndTransferCallback([&endCalls]() { endCalls += 1; });

    hdl.start(&stream.NDTR, buffer, 8, false);
    REQUIRE((stream.CR & DMA_SxCR_CIRC) == 0U);
    REQUIRE(hdl.running() == true);

    hdl.IRQhandler(DMA_LISR_TCIF0);
    REQUIRE(hdl.running() == false);
    REQUIRE(endCalls == 1);
}

TEST_CASE("Transfer error ends a circular stream and wakes the waiting thread",
          "[stm32][dma]")
{
    using namespace miosix;

    DMA_Stream_TypeDef stream = makeStream();
    stream.CR.value = DMA_SxCR_TCIE | DMA_SxCR_TEIE
                    | (1U << DMA_SxCR_MSIZE_Pos);

    int endCalls = 0;
    uint16_t buffer[8];
    StreamHandler hdl(&stream, DMA2_Stream0_IRQn);
    hdl.setEndTransferCallback([&endCalls]() { endCalls += 1; });
    hdl.start(&stream.NDTR, buffer, 8, true);

    Thread *thread = Thread::IRQgetCurrentThread();
    thread->wakeups = 0;

    // The transfer error interrupt fires while the thread waits in sync(): the
    // hardware has already cleared the EN bit at this point.
    Thread::yieldHook() = [&hdl, &stream]() {
        stream.CR.value &= ~DMA_SxCR_EN;
        hdl.IRQhandler(DMA_LISR_TEIF0);
    };

    REQUIRE(hdl.sync() == true);
    Thread::yieldHook() = nullptr;

    REQUIRE(thread->wakeups == 1);
    REQUIRE(endCalls == 1);
    REQUIRE(hdl.running() == false);

    // Stream can be restarted afterwards and behaves as a fresh one
    hdl.start(&stream.NDTR, buffer, 8, true);
    REQUIRE(hdl.running() == true);
    hdl.IRQhandler(DMA_LISR_TCIF0);
    REQUIRE(hdl.running() == true);
    REQUIRE(endCalls == 1);
}

TEST_CASE("Half transfer interrupt wakes the waiting thread", "[stm32][dma]")
{
    using namespace miosix;

    DMA_Stream_TypeDef stream = makeStream();
    stream.CR.value = DMA_SxCR_TCIE | DMA_SxCR_TEIE
                    | (1U << DMA_SxCR_MSIZE_Pos);

    uint16_t buffer[8];
    StreamHandler hdl(&stream, DMA2_Stream0_IRQn);
    hdl.start(&stream.NDTR, buffer, 8, true);

    Thread *thread = Thread::IRQgetCurrentThread();
    thread->wakeups = 0;

    Thread::yieldHook() = [&hdl]() { hdl.IRQhandler(DMA_LISR_HTIF0); };
    REQUIRE(hdl.sync() == true);
    Thread::yieldHook() = nullptr;

    REQUIRE(thread->wakeups == 1);
    REQUIRE(hdl.running() == true);
}
