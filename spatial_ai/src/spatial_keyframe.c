/*
 * SPATIAL-PATTERN-AI (CANVAS)
 * Copyright (c) 2026 sjpupro-lab
 * GitHub: https://github.com/sjpupro-lab/CANVAS
 *
 * Licensed under AGPL v3.
 * Commercial use requires a separate license.
 * See COMMERCIAL_LICENSE.md for details.
 */

#include "spatial_keyframe.h"
#include "spatial_layers.h"
#include "spatial_subtitle.h"   /* SpatialCanvasPool for ai_get_canvas_pool */
#include <string.h>
#include <stdio.h>

#define INITIAL_CAPACITY 64
#define SIMILARITY_THRESHOLD 0.3f

SpatialAI* spatial_ai_create(void) {
    SpatialAI* ai = (SpatialAI*)malloc(sizeof(SpatialAI));
    if (!ai) return NULL;

    ai->kf_count = 0;
    ai->kf_capacity = INITIAL_CAPACITY;
    ai->keyframes = (Keyframe*)calloc(INITIAL_CAPACITY, sizeof(Keyframe));

    ai->df_count = 0;
    ai->df_capacity = INITIAL_CAPACITY;
    ai->deltas = (DeltaFrame*)calloc(INITIAL_CAPACITY, sizeof(DeltaFrame));

    /* Adaptive channel weights start uniform */
    weight_init(&ai->global_weights);

    /* Canvas pool is lazily created on first ai_get_canvas_pool() call */
    ai->canvas_pool = NULL;

    if (!ai->keyframes || !ai->deltas) {
        spatial_ai_destroy(ai);
        return NULL;
    }

    /* Initialize keyframe grids */
    for (uint32_t i = 0; i < ai->kf_capacity; i++) {
        ai->keyframes[i].grid.A = NULL;
    }

    return ai;
}

void spatial_ai_destroy(SpatialAI* ai) {
    if (!ai) return;

    if (ai->keyframes) {
        for (uint32_t i = 0; i < ai->kf_count; i++) {
            SpatialGrid* g = &ai->keyframes[i].grid;
            if (g->A) { free(g->A); g->A = NULL; }
            if (g->R) { free(g->R); g->R = NULL; }
            if (g->G) { free(g->G); g->G = NULL; }
            if (g->B) { free(g->B); g->B = NULL; }
        }
        free(ai->keyframes);
    }

    if (ai->deltas) {
        for (uint32_t i = 0; i < ai->df_count; i++) {
            free(ai->deltas[i].entries);
        }
        free(ai->deltas);
    }

    if (ai->canvas_pool) {
        pool_destroy(ai->canvas_pool);
        ai->canvas_pool = NULL;
    }

    free(ai);
}

/* ── Lazy canvas-pool accessors ── */

SpatialCanvasPool* ai_get_canvas_pool(SpatialAI* ai) {
    if (!ai) return NULL;
    if (!ai->canvas_pool) ai->canvas_pool = pool_create();
    return ai->canvas_pool;
}

void ai_release_canvas_pool(SpatialAI* ai) {
    if (!ai || !ai->canvas_pool) return;
    pool_destroy(ai->canvas_pool);
    ai->canvas_pool = NULL;
}

/* Grow keyframe array if needed */
static int ensure_kf_capacity(SpatialAI* ai) {
    if (ai->kf_count < ai->kf_capacity) return 1;

    uint32_t new_cap = ai->kf_capacity * 2;
    Keyframe* new_kf = (Keyframe*)realloc(ai->keyframes, new_cap * sizeof(Keyframe));
    if (!new_kf) return 0;

    ai->keyframes = new_kf;
    /* Zero new entries */
    memset(&ai->keyframes[ai->kf_capacity], 0,
           (new_cap - ai->kf_capacity) * sizeof(Keyframe));
    ai->kf_capacity = new_cap;
    return 1;
}

/* Grow delta array if needed */
static int ensure_df_capacity(SpatialAI* ai) {
    if (ai->df_count < ai->df_capacity) return 1;

    uint32_t new_cap = ai->df_capacity * 2;
    DeltaFrame* new_df = (DeltaFrame*)realloc(ai->deltas, new_cap * sizeof(DeltaFrame));
    if (!new_df) return 0;

    ai->deltas = new_df;
    memset(&ai->deltas[ai->df_capacity], 0,
           (new_cap - ai->df_capacity) * sizeof(DeltaFrame));
    ai->df_capacity = new_cap;
    return 1;
}

/* Allocate and copy grid channels into keyframe inline grid */
static void keyframe_alloc_grid(Keyframe* kf, const SpatialGrid* src) {
    kf->grid.A = (uint16_t*)malloc(GRID_TOTAL * sizeof(uint16_t));
    kf->grid.R = (uint8_t*)malloc(GRID_TOTAL);
    kf->grid.G = (uint8_t*)malloc(GRID_TOTAL);
    kf->grid.B = (uint8_t*)malloc(GRID_TOTAL);

    memcpy(kf->grid.A, src->A, GRID_TOTAL * sizeof(uint16_t));
    memcpy(kf->grid.R, src->R, GRID_TOTAL);
    memcpy(kf->grid.G, src->G, GRID_TOTAL);
    memcpy(kf->grid.B, src->B, GRID_TOTAL);
}

uint32_t compute_delta(const SpatialGrid* base, const SpatialGrid* target,
                       DeltaEntry* entries, uint32_t max_entries) {
    if (!base || !target || !entries) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < GRID_TOTAL && count < max_entries; i++) {
        int16_t dA = (int16_t)target->A[i] - (int16_t)base->A[i];
        int8_t  dR = (int8_t)((int)target->R[i] - (int)base->R[i]);
        int8_t  dG = (int8_t)((int)target->G[i] - (int)base->G[i]);

        if (dA != 0 || dR != 0 || dG != 0) {
            entries[count].index = i;
            entries[count].diff_A = dA;
            entries[count].diff_R = dR;
            entries[count].diff_G = dG;
            count++;
        }
    }
    return count;
}

void apply_delta(const SpatialGrid* base, const DeltaEntry* entries,
                 uint32_t count, SpatialGrid* out) {
    if (!base || !entries || !out) return;

    grid_copy(out, base);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = entries[i].index;
        if (idx >= GRID_TOTAL) continue;

        int val_A = (int)out->A[idx] + entries[i].diff_A;
        if (val_A < 0) val_A = 0;
        if (val_A > 65535) val_A = 65535;
        out->A[idx] = (uint16_t)val_A;

        int val_R = (int)out->R[idx] + entries[i].diff_R;
        if (val_R < 0) val_R = 0;
        if (val_R > 255) val_R = 255;
        out->R[idx] = (uint8_t)val_R;

        int val_G = (int)out->G[idx] + entries[i].diff_G;
        if (val_G < 0) val_G = 0;
        if (val_G > 255) val_G = 255;
        out->G[idx] = (uint8_t)val_G;
    }
}

uint32_t ai_store_auto(SpatialAI* ai, const char* clause_text, const char* label) {
    if (!ai || !clause_text) return UINT32_MAX;

    /* Encode clause into grid */
    SpatialGrid* input = grid_create();
    if (!input) return UINT32_MAX;

    morpheme_init();
    layers_encode_clause(clause_text, NULL, input);
    update_rgb_directional(input);

    /* If no keyframes yet, store as first keyframe */
    if (ai->kf_count == 0) {
        if (!ensure_kf_capacity(ai)) { grid_destroy(input); return UINT32_MAX; }

        Keyframe* kf = &ai->keyframes[0];
        kf->id = 0;
        if (label) strncpy(kf->label, label, 63);
        kf->label[63] = '\0';
        kf->text_byte_count = (uint32_t)strlen(clause_text);
        keyframe_alloc_grid(kf, input);

        ai->kf_count = 1;
        grid_destroy(input);
        return 0;
    }

    /* Find best matching keyframe */
    float best_sim = -1.0f;
    uint32_t best_id = 0;

    for (uint32_t i = 0; i < ai->kf_count; i++) {
        float sim = cosine_a_only(input, &ai->keyframes[i].grid);
        if (sim > best_sim) {
            best_sim = sim;
            best_id = i;
        }
    }

    if (best_sim >= SIMILARITY_THRESHOLD) {
        /* Store as delta frame */
        if (!ensure_df_capacity(ai)) { grid_destroy(input); return UINT32_MAX; }

        DeltaEntry* entries = (DeltaEntry*)malloc(GRID_TOTAL * sizeof(DeltaEntry));
        if (!entries) { grid_destroy(input); return UINT32_MAX; }

        uint32_t delta_count = compute_delta(&ai->keyframes[best_id].grid, input,
                                             entries, GRID_TOTAL);

        DeltaFrame* df = &ai->deltas[ai->df_count];
        df->id = ai->df_count;
        df->parent_id = best_id;
        if (label) strncpy(df->label, label, 63);
        df->label[63] = '\0';
        df->count = delta_count;
        DeltaEntry* shrunk = (DeltaEntry*)realloc(entries, delta_count * sizeof(DeltaEntry));
        df->entries = shrunk ? shrunk : entries;
        df->change_ratio = (float)delta_count / (float)grid_active_count(input);

        /* Adaptive feedback: good structural match → boost the channel
         * that most contributed. Compute per-channel similarities
         * between input and matched parent keyframe. */
        {
            SpatialGrid* parent = &ai->keyframes[best_id].grid;
            float sA = channel_sim_A(input, parent);
            float sR = channel_sim_R(input, parent);
            float sG = channel_sim_G(input, parent);
            float sB = channel_sim_B(input, parent);
            weight_update(&ai->global_weights, sA, sR, sG, sB);
        }

        ai->df_count++;
        grid_destroy(input);
        return df->id | 0x80000000u; /* high bit indicates delta */
    } else {
        /* Store as new keyframe */
        if (!ensure_kf_capacity(ai)) { grid_destroy(input); return UINT32_MAX; }

        uint32_t new_id = ai->kf_count;
        Keyframe* kf = &ai->keyframes[new_id];
        kf->id = new_id;
        if (label) strncpy(kf->label, label, 63);
        kf->label[63] = '\0';
        kf->text_byte_count = (uint32_t)strlen(clause_text);
        keyframe_alloc_grid(kf, input);

        /* Adaptive feedback for "novel" input: compare against the
         * nearest existing keyframe to learn which channel saw it as
         * novel (i.e. produced low similarity). Channels that were
         * already low-similarity have done their job in distinguishing
         * the new pattern; they earn a small boost. */
        if (best_id < ai->kf_count - 1) {  /* -1 since we just added */
            SpatialGrid* nearest = &ai->keyframes[best_id].grid;
            float sA = 1.0f - channel_sim_A(input, nearest);
            float sR = 1.0f - channel_sim_R(input, nearest);
            float sG = 1.0f - channel_sim_G(input, nearest);
            float sB = 1.0f - channel_sim_B(input, nearest);
            weight_update(&ai->global_weights, sA, sR, sG, sB);
        }

        ai->kf_count++;
        grid_destroy(input);
        return new_id;
    }
}

uint32_t ai_force_keyframe(SpatialAI* ai, const char* clause_text, const char* label) {
    if (!ai || !clause_text) return UINT32_MAX;

    SpatialGrid* input = grid_create();
    if (!input) return UINT32_MAX;

    morpheme_init();
    layers_encode_clause(clause_text, NULL, input);
    update_rgb_directional(input);

    if (!ensure_kf_capacity(ai)) { grid_destroy(input); return UINT32_MAX; }

    uint32_t new_id = ai->kf_count;
    Keyframe* kf = &ai->keyframes[new_id];
    kf->id = new_id;
    if (label) strncpy(kf->label, label, 63);
    kf->label[63] = '\0';
    kf->text_byte_count = (uint32_t)strlen(clause_text);
    keyframe_alloc_grid(kf, input);

    ai->kf_count++;
    grid_destroy(input);
    return new_id;
}

uint32_t ai_predict(SpatialAI* ai, const char* input_text, float* out_similarity) {
    if (!ai || !input_text || ai->kf_count == 0) {
        if (out_similarity) *out_similarity = 0.0f;
        return UINT32_MAX;
    }

    /* Encode input */
    SpatialGrid* input = grid_create();
    if (!input) {
        if (out_similarity) *out_similarity = 0.0f;
        return UINT32_MAX;
    }

    morpheme_init();
    layers_encode_clause(input_text, NULL, input);
    update_rgb_directional(input);

    /* 2-stage matching pipeline */
    uint32_t n = ai->kf_count;
    Candidate* pool = (Candidate*)malloc(n * sizeof(Candidate));
    if (!pool) {
        grid_destroy(input);
        if (out_similarity) *out_similarity = 0.0f;
        return UINT32_MAX;
    }

    /* Step 1: Overlap coarse */
    for (uint32_t i = 0; i < n; i++) {
        pool[i].id = i;
        pool[i].score = (float)overlap_score(input, &ai->keyframes[i].grid);
    }
    topk_select(pool, n, TOP_K);

    /* Step 2: RGB-weighted cosine on top-K */
    uint32_t eval_count = (n < TOP_K) ? n : TOP_K;
    for (uint32_t i = 0; i < eval_count; i++) {
        pool[i].score = cosine_rgb_weighted(input, &ai->keyframes[pool[i].id].grid);
    }
    topk_select(pool, eval_count, 1);

    uint32_t best_id = pool[0].id;
    float best_sim = pool[0].score;

    free(pool);
    grid_destroy(input);

    if (out_similarity) *out_similarity = best_sim;
    return best_id;
}
