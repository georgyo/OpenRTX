/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FAKE_MIOSIX_H
#define FAKE_MIOSIX_H

/*
 * Minimal stand-in for the Miosix kernel API used by the STM32 DMA stream
 * driver, for host unit testing. Thread::yield() runs a test-provided hook,
 * which allows a test to simulate an interrupt firing while a thread waits.
 */

#include <functional>

namespace miosix
{

class Thread
{
public:
    /**
     * Hook executed by yield(), settable by tests.
     */
    static std::function<void()> &yieldHook()
    {
        static std::function<void()> hook;
        return hook;
    }

    /**
     * The one and only "current" thread of the fake kernel.
     */
    static Thread *IRQgetCurrentThread()
    {
        static Thread current;
        return &current;
    }

    static void IRQwait()
    {
    }

    static void yield()
    {
        if (yieldHook())
            yieldHook()();
    }

    void IRQwakeup()
    {
        wakeups += 1;
    }

    int IRQgetPriority() const
    {
        return 0;
    }

    int wakeups = 0;
};

class FastInterruptDisableLock
{
public:
    // Non-trivial so that a lock object used only for its scope is not
    // reported as an unused variable.
    FastInterruptDisableLock()
    {
    }

    ~FastInterruptDisableLock()
    {
    }
};

class FastInterruptEnableLock
{
public:
    explicit FastInterruptEnableLock(FastInterruptDisableLock &lock)
    {
        (void)lock;
    }
};

class Scheduler
{
public:
    static void IRQfindNextThread()
    {
    }
};

} // namespace miosix

#endif /* FAKE_MIOSIX_H */
