/* warm_profile.h — Fluxa Warm Path: compact execution profile
 *
 * Inspired by TurboQuant (Google Research, 2025): WHT path signature +
 * QJL 1-bit residual per slot. Applied to Fluxa's AST execution state.
 *
 * Design (v0.14 — single contiguous block, power-of-2 growth):
 *
 *   WarmProfile.funcs = one malloc'd WarmFunc[] block.
 *   Starts at WARM_FUNC_INIT_CAP (32). When > 75% full: realloc × 2.
 *   Doubling is a shift — O(1) amortized, zero per-function overhead.
 *   One pointer indirection total (not one per function).
 *   Cache-friendly: all WarmFuncs contiguous, linear probe stays warm.
 *
 *   No obs_calls / cold-lock: Fluxa is strongly typed. Every function
 *   promotes after WARM_STABLE_RUNS (2) consecutive stable executions.
 *   No cap: grows automatically. warm_func_cap in fluxa.toml sets the
 *   initial allocation only (like prst_cap) — not a ceiling.
 */
#ifndef FLUXA_WARM_PROFILE_H
#define FLUXA_WARM_PROFILE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Type encoding (3 bits) ──────────────────────────────────────────────── */
#define WARM_T_NIL   0u
#define WARM_T_INT   1u
#define WARM_T_FLOAT 2u
#define WARM_T_BOOL  3u
#define WARM_T_STR   4u
#define WARM_T_ARR   5u
#define WARM_T_DYN   6u
#define WARM_T_OTHER 7u

/* ── WarmSlot — 1 byte per AST node with a resolved_offset ──────────────── */
typedef struct {
    uint8_t observed_type : 3;
    uint8_t qjl_guard     : 1;
    uint8_t run_count     : 4;
} WarmSlot;

/* ── WarmFunc — per-function profile ─────────────────────────────────────── */
#define WARM_SLOTS_MAX   256
#define WARM_STABLE_RUNS   2

typedef struct {
    uint64_t  path_sig;
    uint8_t   stable_runs;
    uint8_t   node_count;
    uint8_t   _pad[2];
    uintptr_t fn_id;        /* key: (uintptr_t)fn_node — 0 = empty slot  */
    WarmSlot  slots[WARM_SLOTS_MAX];
} WarmFunc;                 /* 272 bytes per function                     */

/* ── WarmProfile — single contiguous block, power-of-2 growth ───────────── */
#define WARM_FUNC_INIT_CAP 32  /* initial allocation — doubles on demand  */

typedef struct {
    WarmFunc *funcs;   /* one realloc'd block — contiguous, cache-friendly */
    int       count;   /* occupied slots                                    */
    int       cap;     /* current capacity (always power of 2)              */
    int       enabled;
} WarmProfile;

/* ── ValType → warm type tag ─────────────────────────────────────────────── */
static inline uint8_t warm_type_from_val_type(int vtype) {
    switch (vtype) {
        case 0: return WARM_T_NIL;
        case 1: return WARM_T_INT;
        case 2: return WARM_T_FLOAT;
        case 3: return WARM_T_BOOL;
        case 4: return WARM_T_STR;
        case 5: return WARM_T_ARR;
        case 6: return WARM_T_DYN;
        default: return WARM_T_OTHER;
    }
}

/* ── Walsh-Hadamard Transform ────────────────────────────────────────────── */
static inline uint64_t warm_wht_sign(uint64_t type_vec) {
    uint64_t v = type_vec;
    v ^= (v >> 1)  & 0x5555555555555555ULL;
    v ^= (v >> 2)  & 0x3333333333333333ULL;
    v ^= (v >> 4)  & 0x0F0F0F0F0F0F0F0FULL;
    v ^= (v >> 8)  & 0x00FF00FF00FF00FFULL;
    v ^= (v >> 16) & 0x0000FFFF0000FFFFULL;
    v ^= (v >> 32);
    return v;
}

static inline uint64_t warm_build_type_vec(const WarmSlot *slots, int count) {
    uint64_t vec = 0;
    int n = count < 16 ? count : 16;
    for (int i = 0; i < n; i++)
        vec |= ((uint64_t)(slots[i].observed_type & 0x7u)) << (i * 4);
    return vec;
}

/* ── WarmProfile lifecycle ───────────────────────────────────────────────── */
static inline int warm_next_pow2(int n) {
    int p = WARM_FUNC_INIT_CAP;
    while (p < n) p <<= 1;  /* shift = double */
    return p;
}

static inline void warm_profile_init(WarmProfile *wp, int initial_cap) {
    int cap = warm_next_pow2(initial_cap > 0 ? initial_cap : WARM_FUNC_INIT_CAP);
    wp->funcs   = (WarmFunc *)calloc((size_t)cap, sizeof(WarmFunc));
    wp->cap     = wp->funcs ? cap : 0;
    wp->count   = 0;
    wp->enabled = 0;
}

static inline void warm_profile_free(WarmProfile *wp) {
    if (wp) { free(wp->funcs); wp->funcs = NULL; wp->cap = 0; wp->count = 0; }
}

/* Grow the block by doubling (one realloc, rehash in-place).
 * Called only when > 75% full — amortized O(1). */
static inline int warm_profile_grow(WarmProfile *wp) {
    int new_cap = wp->cap << 1;  /* double — shift */
    WarmFunc *nb = (WarmFunc *)calloc((size_t)new_cap, sizeof(WarmFunc));
    if (!nb) return 0;
    /* Rehash all occupied entries into the new block */
    for (int i = 0; i < wp->cap; i++) {
        if (!wp->funcs[i].fn_id) continue;
        uint32_t h = (uint32_t)((wp->funcs[i].fn_id ^ (wp->funcs[i].fn_id >> 16))
                                 * 0x45d9f3bU);
        int start = (int)(h & (uint32_t)(new_cap - 1));
        for (int j = 0; j < new_cap; j++) {
            int idx = (start + j) & (new_cap - 1);
            if (!nb[idx].fn_id) { nb[idx] = wp->funcs[i]; break; }
        }
    }
    free(wp->funcs);
    wp->funcs = nb;
    wp->cap   = new_cap;
    return 1;
}

/* Find or create WarmFunc for fn_id. O(1) average, contiguous block. */
static inline WarmFunc *warm_profile_get_func(WarmProfile *wp, uintptr_t fn_id) {
    if (!wp || !wp->funcs || wp->cap < 1) return NULL;
    /* Grow before inserting if > 75% full */
    if (wp->count * 4 >= wp->cap * 3)
        if (!warm_profile_grow(wp)) return NULL;
    uint32_t h = (uint32_t)((fn_id ^ (fn_id >> 16)) * 0x45d9f3bU);
    int start = (int)(h & (uint32_t)(wp->cap - 1));
    for (int i = 0; i < wp->cap; i++) {
        int       idx = (start + i) & (wp->cap - 1);
        WarmFunc *wf  = &wp->funcs[idx];
        if (wf->fn_id == fn_id) return wf;   /* found */
        if (wf->fn_id == 0) {                /* empty — claim */
            memset(wf, 0, sizeof(*wf));
            wf->fn_id = fn_id;
            wp->count++;
            return wf;
        }
    }
    return NULL;
}

/* ── Observation ─────────────────────────────────────────────────────────── */
static inline void warm_record(WarmFunc *wf, int slot_idx, int observed_vtype) {
    if (!wf || slot_idx < 0) return;
    int idx = slot_idx % WARM_SLOTS_MAX;
    uint8_t wt = warm_type_from_val_type(observed_vtype);
    WarmSlot *s = &wf->slots[idx];
    if (s->run_count == 0) {
        s->observed_type = wt; s->qjl_guard = 1; s->run_count = 1;
    } else if (s->observed_type == wt) {
        s->qjl_guard = 1;
        if (s->run_count < 15) s->run_count++;
    } else {
        s->qjl_guard = 0; s->observed_type = wt; s->run_count = 1;
    }
    if (slot_idx >= (int)wf->node_count)
        wf->node_count = (uint8_t)(slot_idx < 255 ? slot_idx + 1 : 255);
}

/* ── WHT signature update ────────────────────────────────────────────────── */
static inline void warm_update_sig(WarmFunc *wf) {
    if (!wf) return;
    uint64_t vec     = warm_build_type_vec(wf->slots, wf->node_count);
    uint64_t new_sig = warm_wht_sign(vec);
    if (new_sig == wf->path_sig) {
        if (wf->stable_runs < 255) wf->stable_runs++;
    } else {
        wf->path_sig    = new_sig;
        wf->stable_runs = 0;
    }
}

/* ── Promotion ───────────────────────────────────────────────────────────── */
static inline int warm_func_is_promoted(const WarmFunc *wf) {
    return wf && wf->stable_runs >= WARM_STABLE_RUNS;
}

/* Observation runs until promotion — no cold-lock in a typed language. */
static inline int warm_func_observing(const WarmFunc *wf) {
    return wf && !warm_func_is_promoted(wf);
}

#endif /* FLUXA_WARM_PROFILE_H */
