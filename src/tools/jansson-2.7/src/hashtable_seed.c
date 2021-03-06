/*
** Copyright 2005-2016  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/* Generate sizeof(uint32_t) bytes of as random data as possible to seed
   the hash function.
*/

#ifdef HAVE_CONFIG_H
#include <jansson_private_config.h>
#endif

#include <stdio.h>
#include <time.h>

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_SCHED_H
#include <sched.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif


#include "jansson.h"


static uint32_t buf_to_uint32(char *data) {
    size_t i;
    uint32_t result = 0;

    for (i = 0; i < sizeof(uint32_t); i++)
        result = (result << 8) | (unsigned char)data[i];

    return result;
}



/* /dev/urandom */
#if !defined(_WIN32) && defined(USE_URANDOM)
static int seed_from_urandom(uint32_t *seed) {
    /* Use unbuffered I/O if we have open(), close() and read(). Otherwise
       fall back to fopen() */

    char data[sizeof(uint32_t)];
    int ok;

#if defined(HAVE_OPEN) && defined(HAVE_CLOSE) && defined(HAVE_READ)
    int urandom;
    urandom = open("/dev/urandom", O_RDONLY);
    if (urandom == -1)
        return 1;

    ok = read(urandom, data, sizeof(uint32_t)) == sizeof(uint32_t);
    close(urandom);
#else
    FILE *urandom;

    urandom = fopen("/dev/urandom", "rb");
    if (!urandom)
        return 1;

    ok = fread(data, 1, sizeof(uint32_t), urandom) == sizeof(uint32_t);
    fclose(urandom);
#endif

    if (!ok)
        return 1;

    *seed = buf_to_uint32(data);
    return 0;
}
#endif

/* Windows Crypto API */

/* gettimeofday() and getpid() */
static int seed_from_timestamp_and_pid(uint32_t *seed) {
#ifdef HAVE_GETTIMEOFDAY
    /* XOR of seconds and microseconds */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *seed = (uint32_t)tv.tv_sec ^ (uint32_t)tv.tv_usec;
#else
    /* Seconds only */
    *seed = (uint32_t)time(NULL);
#endif

    /* XOR with PID for more randomness */
#if   defined(HAVE_GETPID)
    *seed ^= (uint32_t)getpid();
#endif

    return 0;
}

static uint32_t generate_seed() {
    uint32_t seed;
    int done = 0;

#if !defined(_WIN32) && defined(USE_URANDOM)
    if (!done && seed_from_urandom(&seed) == 0)
        done = 1;
#endif


    if (!done) {
        /* Fall back to timestamp and PID if no better randomness is
           available */
        seed_from_timestamp_and_pid(&seed);
    }

    /* Make sure the seed is never zero */
    if (seed == 0)
        seed = 1;

    return seed;
}


volatile uint32_t hashtable_seed = 0;

#if defined(HAVE_ATOMIC_BUILTINS) && (defined(HAVE_SCHED_YIELD) || !defined(_WIN32))
static volatile char seed_initialized = 0;

void json_object_seed(size_t seed) {
    uint32_t new_seed = (uint32_t)seed;

    if (hashtable_seed == 0) {
        if (__atomic_test_and_set(&seed_initialized, __ATOMIC_RELAXED) == 0) {
            /* Do the seeding ourselves */
            if (new_seed == 0)
                new_seed = generate_seed();

            __atomic_store_n(&hashtable_seed, new_seed, __ATOMIC_RELEASE);
        } else {
            /* Wait for another thread to do the seeding */
            do {
#ifdef HAVE_SCHED_YIELD
                sched_yield();
#endif
            } while(__atomic_load_n(&hashtable_seed, __ATOMIC_ACQUIRE) == 0);
        }
    }
}
#elif defined(HAVE_SYNC_BUILTINS) && (defined(HAVE_SCHED_YIELD) || !defined(_WIN32))
void json_object_seed(size_t seed) {
    uint32_t new_seed = (uint32_t)seed;

    if (hashtable_seed == 0) {
        if (new_seed == 0) {
            /* Explicit synchronization fences are not supported by the
               __sync builtins, so every thread getting here has to
               generate the seed value.
            */
            new_seed = generate_seed();
        }

        do {
            if (__sync_bool_compare_and_swap(&hashtable_seed, 0, new_seed)) {
                /* We were the first to seed */
                break;
            } else {
                /* Wait for another thread to do the seeding */
#ifdef HAVE_SCHED_YIELD
                sched_yield();
#endif
            }
        } while(hashtable_seed == 0);
    }
}
#else
/* Fall back to a thread-unsafe version */
void json_object_seed(size_t seed) {
    uint32_t new_seed = (uint32_t)seed;

    if (hashtable_seed == 0) {
        if (new_seed == 0)
            new_seed = generate_seed();

        hashtable_seed = new_seed;
    }
}
#endif
