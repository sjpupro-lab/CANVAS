# SPATIAL-PATTERN-AI (CANVAS)

> A spatial pattern-based AI engine that encodes language as 256x256 pixel grids — treating text like video frames instead of token vectors.

```
 "The cat eats rice."          256x256 Grid (1 clause = 1 frame)
         |                    ┌────────────────────────┐
    UTF-8 bytes               │  ·                     │
         |                    │    ·  ·                 │
   ┌─────▼──────┐            │  · · ·  ·   ·          │
   │  X = byte   │            │     ·    ·              │
   │  Y = position│  ──────►  │  ·    ·                 │
   │  brightness  │            │       ·  ·             │
   │  = weight    │            │  · ·       ·  ·        │
   └─────────────┘            └────────────────────────┘
                               A channel: frequency heatmap
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

### 3. Matching Pipeline

Two-stage search: fast coarse filter, then precise scoring.

```
  Input clause
       │
       ▼
  ┌─────────────────────┐
  │  3-Layer Encoding    │
  │  + RGB Directional   │
  └──────────┬──────────┘
             │
       ┌─────▼──────┐       KF < 100: full overlap scan
       │  Stage 1    │       KF ≥ 100: hash bucket → overlap
       │  Coarse     │
       │  (overlap)  │──── Top-K candidates (K=8)
       └─────┬──────┘
             │
       ┌─────▼──────┐
       │  Stage 2    │       RGB-weighted cosine similarity
       │  Precise    │       + block skip optimization
       │  (cosine)   │──── Best match + similarity %
       └─────────────┘
```

### 4. Keyframe / Delta Storage

Like video codecs: I-frames (full snapshots) and P-frames (diffs only).

```
  Frame 0: [I] "귀여운 고양이가 밥을 먹는다."   topic: animal_meal
  Frame 1: [P] "귀여운 강아지가 물을 먹는다."   topic: animal_meal  (Δ16px from F0)
  Frame 2: [I] "우리는 함께 오랜 세월을 살았다." topic: nostalgia
  Frame 3: [I] "오늘 아침 하늘이 밝다."         topic: sky
  ...
  Frame N: ∞ (unlimited context via frame stacking)

  ┌──────────────────────────────────────────┐
  │  similarity ≥ 0.3 → store as delta (P)   │
  │  similarity < 0.3 → new keyframe (I)     │
  └──────────────────────────────────────────┘
```

### 5. Engine Optimizations

```
  Optimization          │ Result                  │ Speedup
 ────────────────────────┼─────────────────────────┼──────────
  1D aligned memory      │ AVX2/SIMD ready         │  2-5×
  Block skip (16×16)     │ 93% blocks skipped      │  2-4×
                         │ 0.000% accuracy loss    │
  Adaptive Top-K         │ hash buckets for KF≥100 │ 10-50×
  Sparse delta           │ 16 entries = 96 bytes   │ memory
  LRU cache (256 slots)  │ 90% hit rate            │  2-10×
 ────────────────────────┼─────────────────────────┼──────────
  Combined               │ accuracy 100% preserved │ 20-100×
```

## Verified Results

```
  Test Suite: 34/34 PASS

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
├── include/              # Header files
│   ├── spatial_grid.h        # 256×256 grid, encoding/decoding
│   ├── spatial_layers.h      # 3-layer summation engine
│   ├── spatial_morpheme.h    # Korean morpheme analyzer
│   ├── spatial_keyframe.h    # Keyframe / delta / frame
│   ├── spatial_match.h       # Cosine similarity, matching
│   └── spatial_context.h     # Context frames, LRU cache
├── src/                  # Source files
│   ├── spatial_grid.c
│   ├── spatial_layers.c
│   ├── spatial_morpheme.c
│   ├── spatial_keyframe.c
│   ├── spatial_match.c
│   └── spatial_context.c
├── dict/                 # Korean dictionaries
│   ├── nouns.txt
│   ├── verbs.txt
│   ├── adjectives.txt
│   ├── particles.txt
│   └── endings.txt
├── tests/                # 7 test suites, 34 tests
│   ├── test_grid.c
│   ├── test_layers.c
│   ├── test_morpheme.c
│   ├── test_match.c
│   ├── test_keyframe.c
│   ├── test_context.c
│   └── test_integration.c
├── Makefile
├── SPEC.md                   # Core specification v3.0
└── SPEC-ENGINE.md            # Engine optimization spec
```

## Build & Test

```bash
cd spatial_ai

# Build
make all

# Run all tests
make test

# Clean
make clean
```

**Requirements:** GCC (C11), Make, Linux/macOS/Windows (MinGW)

## Key Properties

```
                  Traditional LLM           SPATIAL-PATTERN-AI
 ─────────────────────────────────────────────────────────────
  Encoding       token → vector → matrix    byte → pixel → 256×256
  Parameters     fixed-size matrix          unlimited frame stack
  Context        bounded window (128K)      unlimited (disk-bound)
  Interpretability opaque weights           visible heatmap
  Learning       full retrain               incremental delta
  Frame memory   —                          320 KB / frame
```

## Unique Properties

- **Interpretability**: Open the heatmap to visually confirm what the AI is remembering
- **Unlimited Parameters**: No upper bound — parameters grow as frames accumulate
- **Unlimited Context**: Scales as far as disk allows
- **Incremental Learning**: New data adds only a delta or a new frame — no full retrain
- **Rewind / Branching**: Traverse delta chains backwards to trace the learning history
- **Lightweight**: 320 KB per frame; engine core is a few MB — runs on Termux and embedded systems

## License / 라이선스

This project is dual-licensed under the GNU Affero General Public License v3.0 (AGPL-3.0) for open-source use, and a separate Commercial License for proprietary use.

이 프로젝트는 오픈소스 사용을 위한 GNU Affero General Public License v3.0 (AGPL-3.0)과 상업적 사용을 위한 별도의 상업용 라이선스로 이중 라이선스됩니다.

### Commercial Use / 상업적 이용

If you want to use this engine in a commercial product, SaaS, API service, or any closed-source system,
you must obtain a commercial license.

이 엔진을 상업용 제품, SaaS, API 서비스 또는 비공개 소스 시스템에 사용하려면
별도의 상업용 라이선스가 필요합니다.

See [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md) for details.
자세한 내용은 [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md)를 참조하십시오.

Contact / 문의: https://github.com/sjpupro-lab/CANVAS

---

## Usage Restrictions / 사용 제한

This software must NOT be used for: / 이 소프트웨어는 다음 목적으로 사용할 수 없습니다:

- Illegal content / 불법 콘텐츠
- Exploitation of minors / 미성년자 착취
- Harmful or abusive applications / 유해하거나 악용적인 애플리케이션

The authors are not responsible for misuse.
저자는 오용에 대해 책임을 지지 않습니다.
