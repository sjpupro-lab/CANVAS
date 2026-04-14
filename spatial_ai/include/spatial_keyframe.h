/*
 * SPATIAL-PATTERN-AI (CANVAS)
 * Copyright (c) 2026 sjpupro-lab
 * GitHub: https://github.com/sjpupro-lab/CANVAS
 *
 * Licensed under AGPL v3.
 * Commercial use requires a separate license.
 * See COMMERCIAL_LICENSE.md for details.
 */

#ifndef SPATIAL_KEYFRAME_H
#define SPATIAL_KEYFRAME_H

#include "spatial_grid.h"
#include "spatial_match.h"

/* Keyframe (I-Frame): full snapshot */
typedef struct {
    uint32_t    id;
    char        label[64];
    SpatialGrid grid;  /* inline grid (channels point to allocated memory) */
    uint32_t    text_byte_count;
} Keyframe;

/* Delta entry: sparse format (SPEC-ENGINE Phase D) */
typedef struct {
    uint32_t index;     /* y * 256 + x */
    int16_t  diff_A;
    int8_t   diff_R;
    int8_t   diff_G;
} DeltaEntry;  /* 8 bytes */

/* RLE delta entry (for 4096 scale) */
typedef struct {
    uint32_t start;
    uint16_t length;
    int16_t  diff;
} RLEDelta;  /* 8 bytes */

/* Delta frame (P-Frame) */
typedef struct {
    uint32_t     id;
    uint32_t     parent_id;
    char         label[64];
    uint32_t     count;
    DeltaEntry*  entries;
    float        change_ratio;
} DeltaFrame;

/* Main AI engine structure.
 * Named struct (SpatialAI_) so spatial_match.h can forward-declare it
 * for the cascade API without a circular include. */
struct SpatialCanvasPool_;  /* fwd-decl; see spatial_subtitle.h */
typedef struct SpatialAI_ {
    Keyframe*     keyframes;
    uint32_t      kf_count;
    uint32_t      kf_capacity;
    DeltaFrame*   deltas;
    uint32_t      df_count;
    uint32_t      df_capacity;

    /* Adaptive channel weights (SPEC §5: dynamic RGB embedding).
     * Initialised to (1, 1, 1, 1) by spatial_ai_create; updated
     * automatically after each ai_store_auto by the engine. */
    ChannelWeight global_weights;

    /* Optional canvas pool (SPEC §6 + SubtitleTrack). NULL until
     * ai_get_canvas_pool(ai) is called. */
    struct SpatialCanvasPool_* canvas_pool;
} SpatialAI;

/* Forward declarations that avoid pulling spatial_subtitle.h into
 * every translation unit that needs SpatialAI. */
struct SpatialCanvasPool_* ai_get_canvas_pool(SpatialAI* ai);  /* lazy create */
void                       ai_release_canvas_pool(SpatialAI* ai); /* destroy+NULL */

/* Create/destroy engine */
SpatialAI* spatial_ai_create(void);
void       spatial_ai_destroy(SpatialAI* ai);

/* Store a clause: auto-detect keyframe vs delta (threshold 0.3).
   Returns the stored frame ID. */
uint32_t ai_store_auto(SpatialAI* ai,
                       const char* clause_text,
                       const char* label);

/* Always store a clause as a new keyframe, bypassing the delta
   decision. Needed when callers require a 1-1 clause ↔ keyframe
   mapping (e.g. context frames for QA retrieval).
   Returns the new keyframe ID (== ai->kf_count - 1 on success),
   or UINT32_MAX on failure. */
uint32_t ai_force_keyframe(SpatialAI* ai,
                           const char* clause_text,
                           const char* label);

/* Compute delta between two grids.
   Returns number of changed pixels. entries must be pre-allocated. */
uint32_t compute_delta(const SpatialGrid* base, const SpatialGrid* target,
                       DeltaEntry* entries, uint32_t max_entries);

/* Apply delta to reconstruct target from base */
void apply_delta(const SpatialGrid* base, const DeltaEntry* entries,
                 uint32_t count, SpatialGrid* out);

/* Predict: find best matching keyframe for input text.
   Returns keyframe ID, writes similarity to out_similarity. */
uint32_t ai_predict(SpatialAI* ai,
                    const char* input_text,
                    float* out_similarity);

#endif /* SPATIAL_KEYFRAME_H */
