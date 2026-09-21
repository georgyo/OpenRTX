/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef THREADS_H
#define THREADS_H

#include <stddef.h>

/**
 * Threads' stack sizes
 */
#define UI_THREAD_STKSIZE     2048
#define RTX_THREAD_STKSIZE    512
#define CODEC2_THREAD_STKSIZE 16384
#define AUDIO_THREAD_STKSIZE  512

/*
 * DMR audio thread (core/dmr_audio.c): sized for the in-tree vocoder
 * backends, null and loopback, which keep a 27 byte payload, the DSP state
 * of the microphone conditioning and a few scalars on the stack (the stream
 * buffers are static or heap allocated). An out-of-tree backend needing more
 * documents its own figure and this value is raised with it.
 */
#define DMR_AUDIO_THREAD_STKSIZE 4096

/**
 * Thread priority levels, UNIX-like: lower level, higher thread priority
 */
#ifdef _MIOSIX
#define THREAD_PRIO_RT      0
#define THREAD_PRIO_HIGH    1
#define THREAD_PRIO_NORMAL  2
#define THREAD_PRIO_LOW     3
#endif

/**
 * Spawn all the threads for the various functionalities.
 */
void create_threads();

#endif /* THREADS_H */
