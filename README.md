# SPATIAL-PATTERN-AI (CANVAS)

> A spatial pattern-based AI engine that encodes language as 256×256 pixel grids — treating text like video frames instead of token vectors.

```
 "The cat eats rice."          256×256 Grid (1 clause = 1 tile)
         |                    ┌────────────────────────┐
    UTF-8 bytes               │  ·                     │
         |                    │    ·  ·                 │
   ┌─────▼──────┐            │  · · ·  ·   ·          │
   │  X = byte   │            │     ·    ·              │
   │  Y = position│  ──────►  │  ·    ·                 │
   │  brightness  │            │       ·  ·             │
   │  = weight    │            └────────────────────────┘
   └─────────────┘             A channel: frequency heatmap
```

## How It Works

### 1. Three-Layer Bitmap Summation

Each clause is encoded through 3 independent layers, then stacked:

```
  Layer         Target           Weight    What it captures
 ─────────────────────────────────────────────────────────
  Base          all bytes          +1      every byte position
  Word          space-split words  +2      word-level emphasis
  Morpheme      noun/verb/adj...   +1      linguistic structure
 ─────────────────────────────────────────────────────────
  Combined = Base + Word + Morpheme    (max brightness = 4)
```

```
  Verified:  "귀여운 고양이가 밥을 먹는다."
  ─────────────────────────────────────────
  Base layer:      40 px, max 1, total  40
  Word layer:      37 px, max 2, total  74
  Morpheme layer:  37 px, max 1, total  37
  Combined:        40 px, max 4, total 151
                                        ↑
            Conservation: 151 = 40 + 74 + 37  ✓
```

### 2. RGBA Channels

```
  Channel │ Role         │ Type    │ Description
 ─────────┼──────────────┼─────────┼──────────────────────────
    A     │ Brightness   │ uint16  │ Byte frequency (3-layer sum)
    R     │ Semantic     │ uint8   │ Meaning similarity (AI-mapped)
    G     │ Function     │ uint8   │ Part-of-speech / grammar
    B     │ Extended     │ uint8   │ Context, tense, emotion
```

R/G/B values are **not hardcoded** — the AI dynamically maps them through directional diffusion:

```
  R ← diagonal neighbors (↗↘↙↖)   α = 0.05   morpheme/semantic
  G ← vertical neighbors  (↑↓)     β = 0.08   word substitution
  B ← horizontal neighbors (←→)    γ = 0.03   clause ordering
```

### 3. Multi-Tile Canvas (Scaling Structure)

Instead of storing each clause as a standalone 256×256 frame, up to 32 clauses are packed into a single **2048×1024 canvas** arranged in an 8×4 tile grid:

```
  +-------+-------+-------+-------+-------+-------+-------+-------+
  |slot 0 |slot 1 |slot 2 |slot 3 |slot 4 |slot 5 |slot 6 |slot 7 |  y   0–255
  +-------+-------+-------+-------+-------+-------+-------+-------+
  |slot 8 |slot 9 |slot10 |slot11 |slot12 |slot13 |slot14 |slot15 |  y 256–511
  +-------+-------+-------+-------+-------+-------+-------+-------+
  |slot16 |slot17 |slot18 |slot19 |slot20 |slot21 |slot22 |slot23 |  y 512–767
  +-------+-------+-------+-------+-------+-------+-------+-------+
  |slot24 |slot25 |slot26 |slot27 |slot28 |slot29 |slot30 |slot31 |  y 768–1023
  +-------+-------+-------+-------+-------+-------+-------+-------+
```

Benefits of the canvas layout:
- **Cross-boundary RGB diffusion** — B-channel flows horizontally between adjacent clause tiles, giving each clause context from its neighbours.
- **Better RLE compression** — related clauses placed in adjacent tiles produce contiguous identical rows, compressing efficiently.
- **Spatial retrieval** — a query tile is compared against every slot; R/G/B from neighbour tiles inform the match.

### 4. DataType Classification

Every clause is automatically classified before being placed into a canvas. This controls how strongly RGB diffusion crosses slot boundaries:

```
  DataType  │ Condition                  │ Boundary weight
 ───────────┼────────────────────────────┼─────────────────
  PROSE     │ len > 150, narrative text  │ 0.5  (strong flow)
  DIALOG    │ len 30–150, conversational │ 0.3
  CODE      │ special-char ratio > 15%  │ 0.1
  SHORT     │ len < 30, single words     │ 0.02 (isolated)
```

Canvases in a pool are **type-homogeneous**: the pool routes each clause to a canvas of the matching type, creating a new canvas when needed.

### 5. Matching Pipeline

Four-step search with type-jump shortcuts:

```
  Input clause
       │
       ▼
  ┌────────────────────────┐
  │  3-Layer Encoding       │
  │  + RGB Directional      │
  │  + DataType Detection   │
  └───────────┬────────────┘
              │
       ┌──────▼──────────────────────────────────────┐
       │  Step 1: A-cosine scan (same type slots)     │
       │  Step 2: R×G score (same type slots)         │
       │  Step 3: B×A score (same type slots)         │
       │  Step 4: Fallback to other-type slots        │
       └──────┬──────────────────────────────────────┘
              │
        Best (canvas_id, slot_id) + similarity %
```

For the flat keyframe index, a two-stage search is used:

```
       KF < 100: full overlap scan → Top-K → cosine
       KF ≥ 100: hash bucket → overlap → Top-K → cosine
```

### 6. Keyframe / Delta Storage

Like video codecs: I-frames (full snapshots) and P-frames (diffs only).
Scene-change detection decides the frame type using an adaptive EMA threshold (inspired by x264):

```
  Canvas 0: [I] "귀여운 고양이가 밥을 먹는다."   type: DIALOG
  Canvas 1: [P] "귀여운 강아지가 물을 먹는다."   type: DIALOG  (Δ16px from C0)
  Canvas 2: [I] "def foo(): return 42"           type: CODE
  ...
  Canvas N: ∞ (unlimited via canvas pool)

  ┌──────────────────────────────────────────────────────────────┐
  │  changed blocks ≥ 50% → new IFRAME (scene change)           │
  │  changed blocks <  50% → PFRAME (delta vs best reference)   │
  └──────────────────────────────────────────────────────────────┘
```

Delta entries are stored in the most compact format automatically:

```
  Sparse format (6 B/entry): uint32 index + int16 diff_A
  RLE format    (8 B/entry): uint32 start + uint16 length + int16 diff
  → whichever is smaller is chosen at write time
```

### 7. SubtitleTrack

A metadata-only index layer over canvas slots — like a video subtitle track.
It never affects matching scores; it lets queries **jump directly to slots of the relevant DataType** instead of scanning all stored slots.

```
  SubtitleEntry { type, topic_hash, canvas_id, slot_id, byte_length }

  pool_match() flow:
    detect type of query →
    lookup subtitle_track_ids_of_type() →
    A-cosine → R×G → B×A → (fallback: other types)
```

### 8. Model I/O (SPAI v3 Format)

Binary save/load in an append-only tagged-record format:

```
  File header (32 bytes):
    magic[4] = "SPAI"   version = 3
    kf_count  uint32    df_count  uint32
    reserved  3 × uint32

  Record tags:
    0x01  Keyframe  (327 752 bytes body: id + label + A/R/G/B grids)
    0x02  Delta     (variable: id + parent_id + sparse entries)
    0x03  ChannelWeights  (4 × float)
    0x04  Canvas    (full 2048×1024 canvas)
    0x05  SubtitleTrack
```

Incremental save (`ai_save_incremental`) appends only new records, never rewriting existing data — safe for large models.

### 9. Generation

The engine can **decode learned patterns back to text**:

```
  ai_generate_next(input_text):
    1. encode input_text (3-layer + RGB diffusion)
    2. match_engine → best_kf_id
    3. next keyframe = keyframes[best_kf_id + 1]
    4. grid_decode_text(next keyframe) → output bytes
```

Scoring uses all four channels (SPEC §9.4):

```
  score(y, v) = A_sum[y,v]
              × (1 – |R_mean – in_R| / 255)
              × (1 – |G_mean – in_G| / 255)
              × (1 – |B_mean – in_B| / 255)
```

### 10. Engine Optimizations

```
  Optimization          │ Result                  │ Speedup
 ────────────────────────┼─────────────────────────┼──────────
  1D aligned memory      │ AVX2/SIMD ready         │  2-5×
  Block skip (16×16)     │ 93% blocks skipped      │  2-4×
                         │ 0.000% accuracy loss    │
  Adaptive Top-K         │ hash buckets for KF≥100 │ 10-50×
  Sparse/RLE delta       │ auto-selects smaller    │ memory
  LRU cache (256 slots)  │ 90% hit rate            │  2-10×
 ────────────────────────┼─────────────────────────┼──────────
  Combined               │ accuracy 100% preserved │ 20-100×
```

## Verified Results

```
  Test Suites: 12 suites, all PASS

  Cosine (similar clauses):     78.5%  ✓
  Cosine (different clauses):    0.0%  ✓
  Block skip vs full cosine:   0.000% difference  ✓
  Summation conservation:      151 = 40+74+37  ✓
  Delta entries (similar):     16 px  ✓
  LRU hit rate (4-slot sim):   90%  ✓

  Similarity Matrix:
  ┌──────┬───────┬───────┬───────┐
  │      │  F0   │  F1   │  F2   │
  ├──────┼───────┼───────┼───────┤
  │  F0  │100.0% │  2.7% │  0.0% │
  │  F1  │  2.7% │100.0% │  9.1% │
  │  F2  │  0.0% │  9.1% │100.0% │
  └──────┴───────┴───────┴───────┘

  F0: "귀여운 고양이가 밥을 먹는다."
  F1: "우리는 함께 오랜 세월을 살았다."
  F2: "오늘 아침 하늘이 밝다."
```

## Morpheme Analyzer

Dictionary-based longest-match tokenizer. No external libraries required.

```
  Input                 Output
 ─────────────────────────────────────────────────────
  "귀여운"          →  [adjective: 귀여운]
  "고양이가"        →  [noun: 고양이] + [particle: 가]
  "밥을"            →  [noun: 밥]    + [particle: 을]
  "먹는다."         →  [verb: 먹]    + [ending: 는다] + [punct: .]
```

```
  Dictionary composition:
  ├── Nouns       88  (animals, food, objects, nature, people, abstract)
  ├── Verbs       39  (먹, 가, 오, 보, 하, 되, ...)
  ├── Adjectives  20  (귀여운, 예쁜, 밝은, 아름다운, ...)
  ├── Particles   26  (은/는/이/가/을/를/에서/으로, ...)
  └── Endings     20  (는다/었다/았다/겠다, ...)
```

## Project Structure

```
spatial_ai/
├── include/                  # Header files
│   ├── spatial_grid.h            # 256×256 grid, encoding/decoding
│   ├── spatial_layers.h          # 3-layer summation engine
│   ├── spatial_morpheme.h        # Korean morpheme analyzer
│   ├── spatial_keyframe.h        # Keyframe / delta / flat index
│   ├── spatial_match.h           # Cosine similarity, matching
│   ├── spatial_context.h         # Context frames, LRU cache
│   ├── spatial_canvas.h          # 2048×1024 multi-tile canvas
│   ├── spatial_subtitle.h        # SubtitleTrack + SpatialCanvasPool
│   ├── spatial_generate.h        # Text generation from learned patterns
│   └── spatial_io.h              # Binary model save/load (SPAI v3)
├── src/                      # Source files (mirrors include/)
├── dict/                     # Korean dictionaries
│   ├── nouns.txt
│   ├── verbs.txt
│   ├── adjectives.txt
│   ├── particles.txt
│   └── endings.txt
├── tests/                    # 12 test suites + 4 benchmarks
│   ├── test_grid.c               # Grid encoding
│   ├── test_layers.c             # 3-layer summation
│   ├── test_morpheme.c           # Morpheme analyzer
│   ├── test_match.c              # Matching pipeline
│   ├── test_keyframe.c           # Keyframe / delta storage
│   ├── test_context.c            # LRU context cache
│   ├── test_integration.c        # End-to-end pipeline
│   ├── test_io.c                 # SPAI binary I/O
│   ├── test_cascade.c            # Multi-level cascade matching
│   ├── test_canvas.c             # Canvas tile management
│   ├── test_adaptive.c           # Adaptive Top-K + scene change
│   ├── test_subtitle.c           # SubtitleTrack + pool routing
│   ├── bench_stsb.c              # STS-B semantic similarity benchmark
│   ├── bench_perplexity.c        # Perplexity evaluation
│   ├── bench_word_predict.c      # Next-word prediction benchmark
│   └── bench_qa.c                # QA retrieval benchmark
├── data/                     # Evaluation datasets (download scripts)
│   ├── download_wiki.ps1
│   ├── download_stsb.ps1
│   └── make_qa.ps1
├── Makefile
├── SPEC.md                   # Core specification v3.0
└── SPEC-ENGINE.md            # Engine optimization specification
```

## Build & Test

```bash
cd spatial_ai

# Build all objects
make all

# Run all 12 test suites
make test

# Build benchmarks
make bench

# Run Wikipedia integration test
make wiki
./build/test_wiki data/sample_ko.txt

# Clean build artefacts
make clean
```

**Requirements:** GCC (C11), Make, Linux/macOS/Windows (MinGW)

## Key Properties

```
                  Traditional LLM           SPATIAL-PATTERN-AI
 ─────────────────────────────────────────────────────────────
  Encoding       token → vector → matrix    byte → pixel → 256×256
  Parameters     fixed-size matrix          unlimited canvas stack
  Context        bounded window (128K)      unlimited (disk-bound)
  Interpretability opaque weights           visible heatmap
  Learning       full retrain               incremental delta / canvas
  Storage unit   —                          2 MB / canvas (32 clauses)
```

## Unique Properties

- **Interpretability**: Open the heatmap to visually confirm what the AI is remembering
- **Unlimited Parameters**: Parameters grow as canvases accumulate — no fixed upper bound
- **Unlimited Context**: Scales as far as disk allows; incremental save appends only new records
- **Incremental Learning**: New data adds a delta or a new canvas tile — no full retrain
- **Rewind / Branching**: Traverse delta chains backwards to trace the learning history
- **Type-aware Routing**: DataType classification ensures prose, dialog, code, and short entries are stored and retrieved separately
- **Lightweight**: ~2 MB per canvas (32 clauses); engine core is a few MB — runs on Termux and embedded systems

## License

See [LICENSE](LICENSE) for details.
