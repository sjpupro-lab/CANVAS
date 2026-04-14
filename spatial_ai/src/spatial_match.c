/*
 * SPATIAL-PATTERN-AI (CANVAS)
 * Copyright (c) 2026 sjpupro-lab
 * GitHub: https://github.com/sjpupro-lab/CANVAS
 *
 * Licensed under AGPL v3.
 * Commercial use requires a separate license.
 * See COMMERCIAL_LICENSE.md for details.
 */

#include "spatial_match.h"
#include "spatial_keyframe.h"   /* full SpatialAI definition for cascade */
#include <stdlib.h>
#include <string.h>

/* ── Directional RGB update (§9.2) ── */

void update_rgb_directional(SpatialGrid* grid) {
    if (!grid) return;

    for (int y = 0; y < GRID_SIZE; y++) {
        for (int x = 0; x < GRID_SIZE; x++) {
            uint32_t idx = (uint32_t)(y * GRID_SIZE + x);
            if (grid->A[idx] == 0) continue;

            /* R: diagonal (morpheme/semantic) */
            int dx[4] = {1, 1, -1, -1};
            int dy[4] = {1, -1, 1, -1};
            for (int d = 0; d < 4; d++) {
                int nx = x + dx[d], ny = y + dy[d];
                if (nx >= 0 && nx < GRID_SIZE && ny >= 0 && ny < GRID_SIZE) {
                    uint32_t nidx = (uint32_t)(ny * GRID_SIZE + nx);
                    if (grid->A[nidx] > 0) {
                        int diff = (int)grid->R[nidx] - (int)grid->R[idx];
                        int delta = (int)(ALPHA_R * diff);
                        int new_val = (int)grid->R[idx] + delta;
                        if (new_val < 0) new_val = 0;
                        if (new_val > 255) new_val = 255;
                        grid->R[idx] = (uint8_t)new_val;
                    }
                }
            }

            /* G: vertical (word substitution) */
            for (int d = -1; d <= 1; d += 2) {
                int ny = y + d;
                if (ny >= 0 && ny < GRID_SIZE) {
                    uint32_t nidx = (uint32_t)(ny * GRID_SIZE + x);
                    if (grid->A[nidx] > 0) {
                        int diff = (int)grid->G[nidx] - (int)grid->G[idx];
                        int delta = (int)(BETA_G * diff);
                        int new_val = (int)grid->G[idx] + delta;
                        if (new_val < 0) new_val = 0;
                        if (new_val > 255) new_val = 255;
                        grid->G[idx] = (uint8_t)new_val;
                    }
                }
            }

            /* B: horizontal (clause order) */
            for (int d = -1; d <= 1; d += 2) {
                int nx = x + d;
                if (nx >= 0 && nx < GRID_SIZE) {
                    uint32_t nidx = (uint32_t)(y * GRID_SIZE + nx);
                    if (grid->A[nidx] > 0) {
                        int diff = (int)grid->B[nidx] - (int)grid->B[idx];
                        int delta = (int)(GAMMA_B * diff);
                        int new_val = (int)grid->B[idx] + delta;
                        if (new_val < 0) new_val = 0;
                        if (new_val > 255) new_val = 255;
                        grid->B[idx] = (uint8_t)new_val;
                    }
                }
            }
        }
    }
}

/* ── Overlap score (Coarse filter §9.3) ── */

uint32_t overlap_score(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (a->A[i] > 0 && b->A[i] > 0) count++;
    }
    return count;
}

/* ── RGB weight (§9.4) ── */

float rgb_weight(uint8_t r1, uint8_t r2,
                 uint8_t g1, uint8_t g2,
                 uint8_t b1, uint8_t b2) {
    float dr = fabsf((float)r1 - (float)r2) / 255.0f;
    float dg = fabsf((float)g1 - (float)g2) / 255.0f;
    float db = fabsf((float)b1 - (float)b2) / 255.0f;
    return 1.0f - (0.5f * dr + 0.3f * dg + 0.2f * db);
}

/* ── A-channel only cosine ── */

float cosine_a_only(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;

    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        double va = (double)a->A[i];
        double vb = (double)b->A[i];
        dot    += va * vb;
        norm_a += va * va;
        norm_b += vb * vb;
    }

    if (norm_a == 0.0 || norm_b == 0.0) return 0.0f;
    return (float)(dot / (sqrt(norm_a) * sqrt(norm_b)));
}

/* ── RGB-weighted cosine (§9.4) ── */

float cosine_rgb_weighted(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;

    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        double va = (double)a->A[i];
        double vb = (double)b->A[i];
        if (va > 0.0 && vb > 0.0) {
            float w = rgb_weight(a->R[i], b->R[i],
                                 a->G[i], b->G[i],
                                 a->B[i], b->B[i]);
            dot += va * vb * (double)w;
        }
        norm_a += va * va;
        norm_b += vb * vb;
    }

    if (norm_a == 0.0 || norm_b == 0.0) return 0.0f;
    return (float)(dot / (sqrt(norm_a) * sqrt(norm_b)));
}

/* ── Block summary (SPEC-ENGINE Phase B) ── */

void compute_block_sums(const SpatialGrid* g, BlockSummary* bs) {
    if (!g || !bs) return;

    for (int by = 0; by < BLOCKS; by++) {
        for (int bx = 0; bx < BLOCKS; bx++) {
            uint32_t s = 0;
            for (int y = 0; y < BLOCK; y++) {
                for (int x = 0; x < BLOCK; x++) {
                    s += g->A[(by * BLOCK + y) * GRID_SIZE + (bx * BLOCK + x)];
                }
            }
            bs->sum[by][bx] = s;
        }
    }
}

/* ── Block-skip cosine (SPEC-ENGINE Phase B.3) ── */

float cosine_block_skip(const SpatialGrid* a, const SpatialGrid* b,
                        const BlockSummary* bs_a, const BlockSummary* bs_b) {
    if (!a || !b || !bs_a || !bs_b) return 0.0f;

    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;

    for (int by = 0; by < BLOCKS; by++) {
        for (int bx = 0; bx < BLOCKS; bx++) {
            /* Both blocks contribute to norms even if one is zero */
            int y_start = by * BLOCK;
            int x_start = bx * BLOCK;

            if (bs_a->sum[by][bx] == 0 && bs_b->sum[by][bx] == 0) {
                /* Both zero: no contribution to anything */
                continue;
            }

            for (int y = 0; y < BLOCK; y++) {
                for (int x = 0; x < BLOCK; x++) {
                    uint32_t idx = (uint32_t)((y_start + y) * GRID_SIZE + x_start + x);
                    double va = (double)a->A[idx];
                    double vb = (double)b->A[idx];
                    norm_a += va * va;
                    norm_b += vb * vb;

                    if (bs_a->sum[by][bx] == 0 || bs_b->sum[by][bx] == 0) {
                        /* One side zero: dot product contribution is 0 */
                        continue;
                    }
                    dot += va * vb;
                }
            }
        }
    }

    if (norm_a == 0.0 || norm_b == 0.0) return 0.0f;
    return (float)(dot / (sqrt(norm_a) * sqrt(norm_b)));
}

/* ── Top-K selection (partial sort) ── */

void topk_select(Candidate* pool, uint32_t pool_size, uint32_t k) {
    if (!pool || pool_size <= k) return;

    /* Simple selection sort for top-k */
    for (uint32_t i = 0; i < k && i < pool_size; i++) {
        uint32_t max_idx = i;
        for (uint32_t j = i + 1; j < pool_size; j++) {
            if (pool[j].score > pool[max_idx].score) {
                max_idx = j;
            }
        }
        if (max_idx != i) {
            Candidate tmp = pool[i];
            pool[i] = pool[max_idx];
            pool[max_idx] = tmp;
        }
    }
}

/* ── Hash bucket (SPEC-ENGINE Phase C) ── */

uint32_t grid_hash(const SpatialGrid* g) {
    if (!g) return 0;
    uint32_t h = 0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (g->A[i] > 0) {
            uint32_t x = i % GRID_SIZE;
            h = h * 31 + x;
        }
    }
    return h % NUM_BUCKETS;
}

void bucket_index_init(BucketIndex* idx) {
    if (!idx) return;
    memset(idx, 0, sizeof(BucketIndex));
}

void bucket_index_add(BucketIndex* idx, const SpatialGrid* g, uint32_t kf_id) {
    if (!idx || !g) return;
    uint32_t h = grid_hash(g);
    Bucket* b = &idx->buckets[h];
    if (b->count < 256) {
        b->ids[b->count++] = kf_id;
    }
}

void bucket_candidates(BucketIndex* idx, uint32_t hash,
                       int expand, uint32_t* out, uint32_t* out_count) {
    if (!idx || !out || !out_count) return;
    *out_count = 0;

    for (int d = -expand; d <= expand; d++) {
        uint32_t bi = (hash + d + NUM_BUCKETS) % NUM_BUCKETS;
        Bucket* b = &idx->buckets[bi];
        for (uint32_t i = 0; i < b->count; i++) {
            if (*out_count < 1024) {
                out[(*out_count)++] = b->ids[i];
            }
        }
    }
}

/* ── Channel-pair scoring primitives ─────────────────────── */

float rg_score(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;
    double s = 0.0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (a->A[i] == 0 || b->A[i] == 0) continue;
        double r_sim = 1.0 - fabs((double)a->R[i] - b->R[i]) / 255.0;
        double g_sim = 1.0 - fabs((double)a->G[i] - b->G[i]) / 255.0;
        if (r_sim < 0) r_sim = 0;
        if (g_sim < 0) g_sim = 0;
        s += r_sim * g_sim;
    }
    return (float)s;
}

float bg_score(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;
    double s = 0.0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (a->A[i] == 0 || b->A[i] == 0) continue;
        double b_sim = 1.0 - fabs((double)a->B[i] - b->B[i]) / 255.0;
        double g_sim = 1.0 - fabs((double)a->G[i] - b->G[i]) / 255.0;
        if (b_sim < 0) b_sim = 0;
        if (g_sim < 0) g_sim = 0;
        s += b_sim * g_sim;
    }
    return (float)s;
}

float ba_score(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;
    double s = 0.0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (a->A[i] == 0 || b->A[i] == 0) continue;
        double b_sim = 1.0 - fabs((double)a->B[i] - b->B[i]) / 255.0;
        uint16_t mn = (a->A[i] < b->A[i]) ? a->A[i] : b->A[i];
        if (b_sim < 0) b_sim = 0;
        s += b_sim * (double)mn;
    }
    return (float)s;
}

float ra_score(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;
    double s = 0.0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (a->A[i] == 0 || b->A[i] == 0) continue;
        double r_sim = 1.0 - fabs((double)a->R[i] - b->R[i]) / 255.0;
        uint16_t mn = (a->A[i] < b->A[i]) ? a->A[i] : b->A[i];
        if (r_sim < 0) r_sim = 0;
        s += r_sim * (double)mn;
    }
    return (float)s;
}

/* ── Cascade Step 1: overlap coarse → cosine_a_only → best ── */

static uint32_t cascade_step1_best(SpatialAI* ai, SpatialGrid* input,
                                   float* out_a_sim) {
    uint32_t n = ai->kf_count;
    Candidate* pool = (Candidate*)malloc(n * sizeof(Candidate));
    if (!pool) { if (out_a_sim) *out_a_sim = 0.0f; return 0; }

    /* Stage 1a: overlap coarse over all KFs */
    for (uint32_t i = 0; i < n; i++) {
        pool[i].id = i;
        pool[i].score = (float)overlap_score(input, &ai->keyframes[i].grid);
    }
    uint32_t k = (TOP_K < n) ? TOP_K : n;
    topk_select(pool, n, k);

    /* Stage 1b: A-only cosine on top-K */
    uint32_t best_id = pool[0].id;
    float    best    = -1.0f;
    for (uint32_t i = 0; i < k; i++) {
        float s = cosine_a_only(input, &ai->keyframes[pool[i].id].grid);
        if (s > best) { best = s; best_id = pool[i].id; }
    }

    free(pool);
    if (out_a_sim) *out_a_sim = (best < 0 ? 0.0f : best);
    return best_id;
}

/* ── Public: match_cascade ─────────────────────────────── */

uint32_t match_cascade(SpatialAI* ai, SpatialGrid* input,
                       CascadeMode mode, float* out_similarity) {
    if (!ai || !input || ai->kf_count == 0) {
        if (out_similarity) *out_similarity = 0.0f;
        return 0;
    }

    /* All modes begin with Step 1: A-only match */
    float a_sim = 0.0f;
    uint32_t a_best = cascade_step1_best(ai, input, &a_sim);

    if (mode == CASCADE_SEARCH) {
        if (out_similarity) *out_similarity = a_sim;
        return a_best;
    }

    /* Early return if A match is strong (structurally identical clause) */
    if (a_sim >= CASCADE_STEP1_THRESHOLD) {
        if (out_similarity) *out_similarity = a_sim;
        return a_best;
    }

    /* Step 2: channel-pair scoring over ALL keyframes */
    uint32_t n = ai->kf_count;
    Candidate* pool = (Candidate*)malloc(n * sizeof(Candidate));
    if (!pool) {
        if (out_similarity) *out_similarity = a_sim;
        return a_best;
    }

    for (uint32_t i = 0; i < n; i++) {
        pool[i].id = i;
        pool[i].score = (mode == CASCADE_QA)
            ? rg_score(input, &ai->keyframes[i].grid)
            : bg_score(input, &ai->keyframes[i].grid);
    }
    uint32_t k = (TOP_K < n) ? TOP_K : n;
    topk_select(pool, n, k);

    /* Step 3: rematch top-K with the OTHER channel pair */
    uint32_t final_id = pool[0].id;
    float    final_score = -1.0f;
    for (uint32_t i = 0; i < k; i++) {
        float s = (mode == CASCADE_QA)
            ? ba_score(input, &ai->keyframes[pool[i].id].grid)
            : ra_score(input, &ai->keyframes[pool[i].id].grid);
        if (s > final_score) { final_score = s; final_id = pool[i].id; }
    }

    free(pool);

    if (out_similarity) {
        /* Normalize: report RGB-weighted cosine for consistency */
        *out_similarity = cosine_rgb_weighted(input, &ai->keyframes[final_id].grid);
    }
    return final_id;
}

/* ── Public: match_cascade_topk ────────────────────────── */

/* ── Adaptive channel weights ─────────────────────────── */

void weight_init(ChannelWeight* w) {
    if (!w) return;
    w->w_A = 1.0f;
    w->w_R = 1.0f;
    w->w_G = 1.0f;
    w->w_B = 1.0f;
}

void weight_normalize(ChannelWeight* w) {
    if (!w) return;
    float s = w->w_A + w->w_R + w->w_G + w->w_B;
    if (s <= 0.0f) { weight_init(w); return; }
    float k = 4.0f / s;
    w->w_A *= k;
    w->w_R *= k;
    w->w_G *= k;
    w->w_B *= k;
    /* Guard small drift to keep weights non-negative in [0, 4] */
    if (w->w_A < 0.0f) w->w_A = 0.0f;
    if (w->w_R < 0.0f) w->w_R = 0.0f;
    if (w->w_G < 0.0f) w->w_G = 0.0f;
    if (w->w_B < 0.0f) w->w_B = 0.0f;
}

void weight_update(ChannelWeight* w,
                   float sim_A, float sim_R, float sim_G, float sim_B) {
    if (!w) return;

    /* Winner takes the reward, others get a small decay. */
    float best = sim_A; int idx = 0;
    if (sim_R > best) { best = sim_R; idx = 1; }
    if (sim_G > best) { best = sim_G; idx = 2; }
    if (sim_B > best) { best = sim_B; idx = 3; }

    const float lr       = WEIGHT_LEARNING_RATE;
    const float decay    = lr / 3.0f;

    /* Apply to all four, subtract LR/3 from losers */
    w->w_A -= decay;
    w->w_R -= decay;
    w->w_G -= decay;
    w->w_B -= decay;

    /* Add LR to winner (so net effect for winner = +LR + decay extra = +LR - decay?
       simpler: undo subtract for winner and add LR directly.) */
    switch (idx) {
        case 0: w->w_A += (lr + decay); break;
        case 1: w->w_R += (lr + decay); break;
        case 2: w->w_G += (lr + decay); break;
        case 3: w->w_B += (lr + decay); break;
    }
    weight_normalize(w);
}

/* ── Per-channel similarity helpers ───────────────────── */

/* Average (1 - |Δ|/255) over cells where both A's are > 0.
 * Returns 0 if no co-active cells. */
static float avg_rgb_sim(const uint8_t* a_ch, const uint8_t* b_ch,
                          const uint16_t* a_A, const uint16_t* b_A) {
    double sum = 0.0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (a_A[i] == 0 || b_A[i] == 0) continue;
        sum += 1.0 - fabs((double)a_ch[i] - b_ch[i]) / 255.0;
        n++;
    }
    if (n == 0) return 0.0f;
    return (float)(sum / n);
}

float channel_sim_A(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;
    return cosine_a_only(a, b);
}

float channel_sim_R(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;
    return avg_rgb_sim(a->R, b->R, a->A, b->A);
}

float channel_sim_G(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;
    return avg_rgb_sim(a->G, b->G, a->A, b->A);
}

float channel_sim_B(const SpatialGrid* a, const SpatialGrid* b) {
    if (!a || !b) return 0.0f;
    return avg_rgb_sim(a->B, b->B, a->A, b->A);
}

float adaptive_score(const SpatialGrid* a, const SpatialGrid* b,
                     const ChannelWeight* w) {
    if (!a || !b) return 0.0f;
    ChannelWeight def;
    weight_init(&def);
    if (!w) w = &def;

    float sA = channel_sim_A(a, b);
    float sR = channel_sim_R(a, b);
    float sG = channel_sim_G(a, b);
    float sB = channel_sim_B(a, b);

    return (w->w_A * sA + w->w_R * sR + w->w_G * sG + w->w_B * sB) / 4.0f;
}

/* ── Weighted cascade variants ────────────────────────── */

uint32_t match_cascade_weighted(SpatialAI* ai, SpatialGrid* input,
                                CascadeMode mode, const ChannelWeight* w,
                                float* out_similarity) {
    if (!w) return match_cascade(ai, input, mode, out_similarity);
    if (!ai || !input || ai->kf_count == 0) {
        if (out_similarity) *out_similarity = 0.0f;
        return 0;
    }

    /* Step 1: A-only cascade step, same as match_cascade */
    float a_sim = 0.0f;
    uint32_t a_best = cascade_step1_best(ai, input, &a_sim);

    if (mode == CASCADE_SEARCH) {
        if (out_similarity) *out_similarity = a_sim;
        return a_best;
    }
    if (a_sim >= CASCADE_STEP1_THRESHOLD) {
        if (out_similarity) *out_similarity = a_sim;
        return a_best;
    }

    /* Step 2/3: use adaptive_score directly. The weighted score
       combines all four channels; cascade mode affects which
       candidates we re-rank but scoring is uniform. */
    uint32_t n = ai->kf_count;
    Candidate* pool = (Candidate*)malloc(n * sizeof(Candidate));
    if (!pool) {
        if (out_similarity) *out_similarity = a_sim;
        return a_best;
    }
    for (uint32_t i = 0; i < n; i++) {
        pool[i].id = i;
        pool[i].score = adaptive_score(input, &ai->keyframes[i].grid, w);
    }
    uint32_t k = (TOP_K < n) ? TOP_K : n;
    topk_select(pool, n, k);

    uint32_t final_id = pool[0].id;
    float final_score = pool[0].score;
    free(pool);

    if (out_similarity) *out_similarity = final_score;
    return final_id;
}

uint32_t match_cascade_topk_weighted(SpatialAI* ai, SpatialGrid* input,
                                     CascadeMode mode, const ChannelWeight* w,
                                     uint32_t k, uint32_t* out_ids,
                                     float* out_scores) {
    if (!w) return match_cascade_topk(ai, input, mode, k, out_ids, out_scores);
    if (!ai || !input || !out_ids || !out_scores || ai->kf_count == 0 || k == 0) return 0;

    (void)mode;  /* adaptive top-K ignores mode — it re-ranks by adaptive_score */

    uint32_t n = ai->kf_count;
    if (k > n) k = n;
    Candidate* pool = (Candidate*)malloc(n * sizeof(Candidate));
    if (!pool) return 0;
    for (uint32_t i = 0; i < n; i++) {
        pool[i].id = i;
        pool[i].score = adaptive_score(input, &ai->keyframes[i].grid, w);
    }
    topk_select(pool, n, k);
    for (uint32_t i = 0; i < k; i++) {
        out_ids[i]    = pool[i].id;
        out_scores[i] = pool[i].score;
    }
    free(pool);
    return k;
}

uint32_t match_cascade_topk(SpatialAI* ai, SpatialGrid* input,
                            CascadeMode mode, uint32_t k,
                            uint32_t* out_ids, float* out_scores) {
    if (!ai || !input || !out_ids || !out_scores || ai->kf_count == 0 || k == 0) {
        return 0;
    }

    uint32_t n = ai->kf_count;
    if (k > n) k = n;

    /* Special case: CASCADE_SEARCH returns top-K by A-only cosine */
    if (mode == CASCADE_SEARCH) {
        Candidate* pool = (Candidate*)malloc(n * sizeof(Candidate));
        if (!pool) return 0;
        for (uint32_t i = 0; i < n; i++) {
            pool[i].id = i;
            pool[i].score = cosine_a_only(input, &ai->keyframes[i].grid);
        }
        topk_select(pool, n, k);
        for (uint32_t i = 0; i < k; i++) {
            out_ids[i]    = pool[i].id;
            out_scores[i] = pool[i].score;
        }
        free(pool);
        return k;
    }

    /* Step 2: channel-pair top candidates */
    Candidate* pool2 = (Candidate*)malloc(n * sizeof(Candidate));
    if (!pool2) return 0;
    for (uint32_t i = 0; i < n; i++) {
        pool2[i].id = i;
        pool2[i].score = (mode == CASCADE_QA)
            ? rg_score(input, &ai->keyframes[i].grid)
            : bg_score(input, &ai->keyframes[i].grid);
    }
    /* Keep a larger pool for re-ranking (TOP_K or k*3, whichever is larger) */
    uint32_t K2 = (k * 3 > TOP_K) ? k * 3 : TOP_K;
    if (K2 > n) K2 = n;
    topk_select(pool2, n, K2);

    /* Step 3: rematch the K2 candidates with the OTHER channel pair */
    Candidate* pool3 = (Candidate*)malloc(K2 * sizeof(Candidate));
    if (!pool3) { free(pool2); return 0; }
    for (uint32_t i = 0; i < K2; i++) {
        pool3[i].id = pool2[i].id;
        pool3[i].score = (mode == CASCADE_QA)
            ? ba_score(input, &ai->keyframes[pool2[i].id].grid)
            : ra_score(input, &ai->keyframes[pool2[i].id].grid);
    }
    topk_select(pool3, K2, k);

    for (uint32_t i = 0; i < k; i++) {
        out_ids[i]    = pool3[i].id;
        out_scores[i] = pool3[i].score;
    }

    free(pool2);
    free(pool3);
    return k;
}
