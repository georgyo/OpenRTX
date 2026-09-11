/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include <minmea.h>
#include "drivers/GPS/nmea_rbuf.h"
}

// 70 characters, longer than the RMC below
static const char gsv[] = "$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,"
                          "00,13,06,292,00*74\r\n";

// 68 characters
static const char rmc[] = "$GPRMC,081836,A,3751.65,S,14507.36,E,000.0,360.0,"
                          "130998,011.3,E*62\r\n";

// 20 characters
static const char gll[] = "$GPGLL,,,,,,V,N*64\r\n";

static void putString(struct nmeaRbuf *rbuf, const char *str)
{
    for (const char *p = str; *p != '\0'; p++)
        REQUIRE(nmeaRbuf_putChar(rbuf, *p) == 0);
}

TEST_CASE("NMEA ring buffer sentence extraction", "[gps][nmea]")
{
    struct nmeaRbuf rbuf;
    nmeaRbuf_reset(&rbuf);

    SECTION("Empty buffer yields no sentence")
    {
        char sentence[16];
        REQUIRE(nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence)) == 0);
    }

    SECTION("Sentence is NUL-terminated in a reused destination buffer")
    {
        // Same flow as gps_task(): one destination buffer, reused for every
        // sentence, whose content is whatever was there before.
        char sentence[2 * MINMEA_MAX_LENGTH];
        memset(sentence, 'Z', sizeof(sentence));

        putString(&rbuf, gsv);
        int len = nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence));
        REQUIRE(len == (int)strlen(gsv));
        REQUIRE(sentence[len] == '\0');
        REQUIRE(strcmp(sentence, gsv) == 0);
        REQUIRE(minmea_sentence_id(sentence, false) == MINMEA_SENTENCE_GSV);

        // The shorter RMC must not be polluted by the tail of the GSV
        putString(&rbuf, rmc);
        len = nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence));
        REQUIRE(len == (int)strlen(rmc));
        REQUIRE(sentence[len] == '\0');
        REQUIRE(strcmp(sentence, rmc) == 0);
        REQUIRE(minmea_sentence_id(sentence, false) == MINMEA_SENTENCE_RMC);

        REQUIRE(nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence)) == 0);
    }

    SECTION("Over-long sentence is truncated, terminated and drained")
    {
        // 16-byte destination followed by a canary byte
        char small[16 + 1];
        memset(small, 'Z', sizeof(small));
        small[16] = 0x7E;

        putString(&rbuf, rmc);
        putString(&rbuf, gll);

        REQUIRE(nmeaRbuf_getSentence(&rbuf, small, 16) == -1);
        REQUIRE(small[16] == 0x7E);
        REQUIRE(small[15] == '\0');
        REQUIRE(strncmp(small, rmc, 15) == 0);

        // The rest of the truncated sentence has been removed: the next call
        // returns the following sentence, not the leftovers.
        char sentence[2 * MINMEA_MAX_LENGTH];
        int len = nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence));
        REQUIRE(len == (int)strlen(gll));
        REQUIRE(strcmp(sentence, gll) == 0);
    }

    SECTION("Destination sized exactly for sentence plus terminator")
    {
        char exact[sizeof(rmc)];
        memset(exact, 'Z', sizeof(exact));

        putString(&rbuf, rmc);
        int len = nmeaRbuf_getSentence(&rbuf, exact, sizeof(exact));
        REQUIRE(len == (int)strlen(rmc));
        REQUIRE(strcmp(exact, rmc) == 0);

        // One byte less: no room for the terminator, sentence is truncated
        char short_[sizeof(rmc) - 1 + 1];
        memset(short_, 'Z', sizeof(short_));
        short_[sizeof(short_) - 1] = 0x7E;

        putString(&rbuf, rmc);
        REQUIRE(nmeaRbuf_getSentence(&rbuf, short_, sizeof(rmc) - 1) == -1);
        REQUIRE(short_[sizeof(rmc) - 2] == '\0');
        REQUIRE(short_[sizeof(short_) - 1] == 0x7E);
    }

    SECTION("Zero-sized destination is rejected without touching it")
    {
        char sentence[2 * MINMEA_MAX_LENGTH];
        putString(&rbuf, rmc);
        REQUIRE(nmeaRbuf_getSentence(&rbuf, sentence, 0) == -1);

        // Nothing has been consumed
        int len = nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence));
        REQUIRE(len == (int)strlen(rmc));
        REQUIRE(strcmp(sentence, rmc) == 0);
    }

    SECTION("Sentences inserted with putSentence across the wrap-around")
    {
        char sentence[2 * MINMEA_MAX_LENGTH];

        // Move the write pointer close to the end of the ring buffer
        putString(&rbuf, rmc);
        REQUIRE(nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence))
                == (int)strlen(rmc));

        REQUIRE(nmeaRbuf_putSentence(&rbuf, gsv) == 0);
        REQUIRE(nmeaRbuf_putSentence(&rbuf, gll) == 0);

        int len = nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence));
        REQUIRE(len == (int)strlen(gsv));
        REQUIRE(strcmp(sentence, gsv) == 0);
        REQUIRE(minmea_sentence_id(sentence, false) == MINMEA_SENTENCE_GSV);

        len = nmeaRbuf_getSentence(&rbuf, sentence, sizeof(sentence));
        REQUIRE(len == (int)strlen(gll));
        REQUIRE(strcmp(sentence, gll) == 0);
        REQUIRE(minmea_sentence_id(sentence, false) == MINMEA_SENTENCE_GLL);
    }
}
