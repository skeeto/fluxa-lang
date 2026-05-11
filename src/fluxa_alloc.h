/* fluxa_alloc.h — Hardware simulation memory allocator
 *
 * When FLUXA_SIM_RP2040 or FLUXA_SIM_ESP32 is defined at compile time,
 * all malloc/calloc/realloc/free calls in the runtime are replaced by
 * these wrappers which enforce a hard SRAM cap:
 *
 *   FLUXA_SIM_RP2040 → 264 KB  (RP2040 / Raspberry Pi Pico 4)
 *   FLUXA_SIM_ESP32  → 520 KB  (ESP32 heap available to user code)
 *
 * When the cap is exceeded:
 *   - fluxa_malloc / fluxa_calloc return NULL (same as OOM on real hardware)
 *   - fluxa_realloc returns NULL and does NOT free the original pointer
 *     (caller must treat NULL as failure and keep using old pointer)
 *   - The runtime checks malloc return values and calls rt_error() on NULL
 *
 * Thread-safe: uses _Atomic size_t for the live-byte counter.
 *
 * No-sim build: fluxa_malloc/calloc/realloc/free are thin aliases for the
 * system allocator — zero overhead, zero code path difference.
 *
 * Usage (Makefile):
 *   make FLUXA_SIM=RP2040    → -DFLUXA_SIM_RP2040=1
 *   make FLUXA_SIM=ESP32     → -DFLUXA_SIM_ESP32=1
 *   make                     → no simulation (production build)
 */
#ifndef FLUXA_ALLOC_H
#define FLUXA_ALLOC_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── SRAM caps ────────────────────────────────────────────────────────────── */
#define FLUXA_SRAM_RP2040   (264u * 1024u)   /* 264 KB */
#define FLUXA_SRAM_ESP32    (520u * 1024u)   /* 520 KB */

#if defined(FLUXA_SIM_RP2040)
#  define FLUXA_SIM_ACTIVE  1
#  define FLUXA_SIM_NAME    "RP2040"
#  define FLUXA_SRAM_CAP    FLUXA_SRAM_RP2040
#elif defined(FLUXA_SIM_ESP32)
#  define FLUXA_SIM_ACTIVE  1
#  define FLUXA_SIM_NAME    "ESP32"
#  define FLUXA_SRAM_CAP    FLUXA_SRAM_ESP32
#else
#  define FLUXA_SIM_ACTIVE  0
#endif

#if FLUXA_SIM_ACTIVE

/* ── Live byte counter ────────────────────────────────────────────────────── */
/* We store the allocation size just before the returned pointer so we can
 * recover it on free/realloc without a separate hash table.
 * Layout: [ size_t nbytes | user data ... ]                                 */
#define FLUXA_ALLOC_HEADER  (sizeof(size_t))

/* Use GCC __atomic builtins — C99 compatible, same semantics as _Atomic */
static volatile size_t _fluxa_live_bytes = 0;
#define _FLUXA_LOAD()        __atomic_load_n(&_fluxa_live_bytes, __ATOMIC_SEQ_CST)
#define _FLUXA_ADD(n)        __atomic_fetch_add(&_fluxa_live_bytes, (n), __ATOMIC_SEQ_CST)
#define _FLUXA_SUB(n)        __atomic_fetch_sub(&_fluxa_live_bytes, (n), __ATOMIC_SEQ_CST)

static inline void fluxa_sim_report_oom(size_t requested) {
    fprintf(stderr,
        "[fluxa] SIM(%s): OOM — requested %zu bytes, live=%zu / cap=%u\n",
        FLUXA_SIM_NAME, requested,
        (size_t)_FLUXA_LOAD(),
        FLUXA_SRAM_CAP);
}

static inline void *fluxa_malloc(size_t n) {
    if (n == 0) n = 1;
    size_t total = n + FLUXA_ALLOC_HEADER;
    size_t prev = _FLUXA_ADD(total);
    if (prev + total > FLUXA_SRAM_CAP) {
        _FLUXA_SUB(total);
        fluxa_sim_report_oom(n);
        return NULL;
    }
    void *raw = malloc(total);
    if (!raw) {
        _FLUXA_SUB(total);
        return NULL;
    }
    *(size_t *)raw = n;
    return (char *)raw + FLUXA_ALLOC_HEADER;
}

static inline void *fluxa_calloc(size_t count, size_t size) {
    size_t n = count * size;
    void *p = fluxa_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static inline void *fluxa_realloc(void *ptr, size_t new_n) {
    if (!ptr) return fluxa_malloc(new_n);
    if (new_n == 0) { /* treat as free + return NULL sentinel */
        void *raw  = (char *)ptr - FLUXA_ALLOC_HEADER;
        size_t old_n = *(size_t *)raw;
        _FLUXA_SUB(old_n + FLUXA_ALLOC_HEADER);
        free(raw);
        return NULL;
    }
    size_t old_n  = *(size_t *)((char *)ptr - FLUXA_ALLOC_HEADER);
    size_t new_total = new_n + FLUXA_ALLOC_HEADER;
    size_t old_total = old_n + FLUXA_ALLOC_HEADER;
    /* Check cap before committing */
    size_t live = _FLUXA_LOAD();
    if (live - old_total + new_total > FLUXA_SRAM_CAP) {
        fluxa_sim_report_oom(new_n);
        return NULL;   /* original ptr still valid — caller must keep using it */
    }
    void *raw = (char *)ptr - FLUXA_ALLOC_HEADER;
    void *new_raw = realloc(raw, new_total);
    if (!new_raw) return NULL;
    *(size_t *)new_raw = new_n;
    _FLUXA_ADD(new_total - old_total);
    return (char *)new_raw + FLUXA_ALLOC_HEADER;
}

static inline void fluxa_free(void *ptr) {
    if (!ptr) return;
    void  *raw  = (char *)ptr - FLUXA_ALLOC_HEADER;
    size_t n    = *(size_t *)raw;
    _FLUXA_SUB(n + FLUXA_ALLOC_HEADER);
    free(raw);
}

static inline char *fluxa_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char  *d = (char *)fluxa_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* Instrumentation — for tests to query live heap usage */
static inline size_t fluxa_sim_live_bytes(void) {
    return (size_t)_FLUXA_LOAD();
}
static inline size_t fluxa_sim_cap_bytes(void) {
    return (size_t)FLUXA_SRAM_CAP;
}

#else  /* ── No simulation — thin aliases ────────────────────────────────── */

static inline void *fluxa_malloc(size_t n)                  { return malloc(n); }
static inline void *fluxa_calloc(size_t c, size_t s)        { return calloc(c, s); }
static inline void *fluxa_realloc(void *p, size_t n)        { return realloc(p, n); }
static inline void  fluxa_free(void *p)                     { free(p); }
static inline char *fluxa_strdup(const char *s)             { return s ? strdup(s) : NULL; }
static inline size_t fluxa_sim_live_bytes(void)             { return 0; }
static inline size_t fluxa_sim_cap_bytes(void)              { return 0; }

#endif /* FLUXA_SIM_ACTIVE */
#endif /* FLUXA_ALLOC_H */
