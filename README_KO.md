# SPATIAL-PATTERN-AI (CANVAS)

![Main Hero](main_hero.png)

> 텍스트를 256×256 격자의 밝기 패턴으로 인코딩하는 공간 패턴 기반 AI 엔진입니다.
> 언어를 토큰 벡터가 아니라 비디오 프레임처럼 다룹니다.

```
 "귀여운 고양이가 밥을 먹는다."     256×256 격자  (1절 = 1프레임)
         │                    ┌────────────────────────┐
    UTF-8 바이트               │  ·                     │
         │                    │    ·  ·                │
   ┌─────▼──────┐             │  · · ·  ·   ·         │
   │ X = byte    │             │     ·    ·            │
   │ Y = position│  ──────►   │  ·    ·                │
   │ A = 3-layer │             │       ·  ·            │
   │     sum     │             │  · ·       ·  ·       │
   └─────────────┘             └────────────────────────┘
                                A 채널: 바이트 빈도 히트맵
```

---

## 목차

- [이 프로젝트를 만든 이유](#이-프로젝트를-만든-이유)
- [동작 방식](#동작-방식)
  - [1. 3-레이어 비트맵 합산](#1-3-레이어-비트맵-합산)
  - [2. RGBA 채널](#2-rgba-채널)
  - [3. 키프레임 / 델타 저장](#3-키프레임--델타-저장)
  - [4. 매칭 캐스케이드](#4-매칭-캐스케이드--통합-2단계-코어)
  - [5. 캔버스 풀 (자막 라우팅)](#5-캔버스-풀-자막-라우팅)
- [현재 검증 결과](#현재-검증-결과)
- [빌드 & 실행](#빌드--실행)
- [저장 / 로드](#저장--로드)
- [프로젝트 구조](#프로젝트-구조)
- [엔진 최적화](#엔진-최적화)
- [형태소 분석기](#형태소-분석기)
- [기존 LLM과 비교](#기존-llm과-비교)
- [라이선스](#라이선스)

---

## 이 프로젝트를 만든 이유

기존 LLM은 언어 지식을 고정 크기 가중치 행렬에 압축합니다. 새 데이터를 넣으려면
재학습이 필요하고, 내부 상태는 블랙박스에 가깝습니다.

이 엔진은 반대로 동작합니다.

- **무제한 파라미터** — 절 하나가 프레임 하나이며, 프레임은 제한 없이 누적
- **무제한 컨텍스트** — 디스크 용량 한도까지 확장
- **관측 가능성** — `A` 채널 히트맵을 직접 열어 기억 패턴 확인 가능
- **증분 학습** — 새 텍스트는 델타 1개 또는 키프레임 1개 추가로 반영
- **경량성** — 프레임당 약 320 KB, 엔진 코어 수 MB로 Termux/임베디드에서 동작

대신, 바이트 단위 공간 통계가 충분한 신호를 담는다는 가정에 기반합니다.
현재 벤치마크는 “리트리벌/리콜 기반 메모리 서브스트레이트로는 충분히 유용”에 가깝고,
“LLM 완전 대체”를 주장하지는 않습니다.

---

## 동작 방식

### 1. 3-레이어 비트맵 합산

각 절을 세 개의 독립 레이어로 인코딩한 뒤 `A` 채널에 합산합니다.
가중치는 중첩 구간이 시각적으로 분리되도록 설계했습니다.

| 레이어 | 대상 | 가중치 | 포착 정보 |
|---|---|---|---|
| **Base** | 모든 바이트 | **+1** | 원시 바이트 위치 |
| **Morpheme** | 명사/동사/형용사 등 바이트 구간 | **+3** | 형태소 구조 |
| **Word** | 공백 기준 단어 바이트 | **+5** | 단어 단위 강조 |

합산된 `A` 채널의 중첩 계층:

```
  base only               A = 1
  base + morpheme         A = 4   (1 + 3)
  base + word             A = 6   (1 + 5)
  base + word + morpheme  A = 9   (1 + 3 + 5)  ← 단어 내부 내용 형태소
```

`"귀여운 고양이가 밥을 먹는다."` 기준 현재 수치(`test_layers`):

```
  active  = 40 pixels
  max A   = 9
  total A = 297

  base total   = 40
  word total   = 185
  morph total  = 72
  sum check    = 40 + 185 + 72 = 297   ✓  (보존 성립)
```

### 2. RGBA 채널

| 채널 | 타입 | 역할 | 설정 방식 |
|---|---|---|---|
| **A** | `uint16` | 밝기/중요도 | 3-레이어 합산 |
| **R** | `uint8`  | 의미(내용어/의미축) | POS 시드 + **대각선** 확산 |
| **G** | `uint8`  | 기능(조사/어미/문장기능) | POS 시드 + **수직** 확산 |
| **B** | `uint8`  | 확장(문맥/순서/감정 슬롯) | POS 시드 + **수평** 확산 + EMA 수렴 |

R/G/B는 **고정 테이블이 아닙니다**. 인코딩 후 `update_rgb_directional`이
절 내부 격자에서 채널별 방향으로 확산하고(슬롯 간/절 간 전파 없음),
엔진은 전체 코퍼스 기준 셀 단위 EMA를 유지합니다.

```
  R ← 대각선 이웃   (↗ ↘ ↙ ↖)    α = 0.05   형태소/의미
  G ← 수직 이웃     (↑ ↓)        β = 0.08   단어 치환
  B ← 수평 이웃     (← →)       γ = 0.03   절 순서/문맥
  EMA (3채널 공통)   α = 0.10   저장 누적 기반
```

POS 시드(방향 확산 전):

```
  POS_NOUN     R=40   G=30   B=100
  POS_VERB     R=120  G=40   B=140
  POS_ADJ      R=170  G=35   B=180
  POS_PARTICLE R=8    G=85   B=90
  POS_ENDING   R=12   G=95   B=110
  POS_PUNCT    R=5    G=120  B=60
  POS_UNKNOWN  R=210  G=20   B=200
```

핵심 구현 포인트:

- **읽기/쓰기 버퍼 분리**(`oldR/newR`, `oldG/newG`, `oldB/newB`)로 스캔 방향 편향 제거
- 평균 이웃 델타가 반올림으로 0이 되어도 실제 신호가 있으면 ±1 이동 유지
- RGB 업데이트는 `A`를 수정하지 않고 마스크로만 사용
- B 채널은 POS prior를 가져 `bg_score`, `channel_sim_B`, 전역 EMA에 유효 신호 제공
  (과거 절 단위 해시 오버레이 방식은 패턴을 덮어 제거됨)

### 3. 키프레임 / 델타 저장

비디오 코덱 방식으로 `ai_store_auto`가 절마다 저장 유형을 결정합니다.

```
  first clause                               → new keyframe (I-frame)
  best cosine-A similarity ≥ 0.3 vs. any KF  → delta (P-frame) against that parent
  best similarity < 0.3                      → new keyframe
```

델타는 실제 변경 셀만 저장하는 **희소(sparse)** 형식입니다.

```c
typedef struct {
    uint32_t index;   // y*256 + x
    int16_t  diff_A;
    int8_t   diff_R;
    int8_t   diff_G;
} DeltaEntry;         // 8 bytes
```

`apply_delta(base, entries, count, out)`은 타깃 격자를 비트 단위로 복원합니다.
`test_io` 라운드트립은 700개 절에 대해 save → destroy → load 후
`|sim_before − sim_after| < 0.001`를 검증합니다.

### 4. 매칭 캐스케이드 — 통합 2단계 코어

세 매칭 API(`ai_predict`, `match_engine`, `match_cascade`)는
단일 `spatial_match()`로 통합되었습니다.

```
  query clause
      │
      ▼
  ┌─────────────────────────┐
  │ encode (3 layers)        │
  │ update_rgb_directional   │
  │ ema_apply                │
  └───────────┬─────────────┘
              │
        Step 1: coarse          if (bucket_idx && KF ≥ 100) → hash-bucket hop
              │                 else → full overlap_score scan
              │                 topk_select → Top-K (K=8)
              ▼
        Step 2: precise         mode-specific scorer:
              │                 MATCH_PREDICT  → cosine_rgb_weighted
              │                 MATCH_SEARCH   → cosine_a_only
              │                 MATCH_QA       → rg_score   (0..1)
              │                 MATCH_GENERATE → bg_score   (0..1)
              ▼
        best id + similarity + topk[]
```

`MatchContext`는 선택적 bucket index와 block-skip/frame-cache 연동 예약 슬롯을 갖고,
`MatchResult`는 `best_id`, `best_score`, 최종 K개 후보 정렬 `topk[]`를 반환합니다.

채널 쌍 스코어는 모두 **[0, 1]** 범위의 평균값(co-active 셀 기준)이므로,
코사인 정규화 스코어와 임계치 비교가 전 경로에서 일관됩니다.

### 5. 캔버스 풀 (자막 라우팅)

절 단위 256×256 격자 위에 32 슬롯을 타일링한 **2048×1024 Canvas**를 둡니다.
`SubtitleTrack`은 각 절에 대해 `(DataType, canvas_id, slot_id)`를 기록하므로,
`pool_match`가 질의 타입(문장/대화/코드/짧은문) 슬롯으로 직접 점프한 뒤
동일한 4단계 채널 페어 캐스케이드를 수행할 수 있습니다.

즉 “H.264 장면 전환” 유사 동작을 합니다.
캔버스는 `KEYFRAME` 또는 `DELTA-of-parent-canvas`가 될 수 있고,
save/load 시 `parent_canvas_id + changed_ratio + classified`를 보존합니다.

---

## 현재 검증 결과

아래 결과는 이 브랜치(`claude/refactor-canvas-spatial-ai-FJt1Y`)에서
`make test`로 재현 가능합니다.

### 테스트 스위트

```
  test_grid         6/6
  test_morpheme     5/5
  test_layers       3/3
  test_match        5/5
  test_keyframe     6/6
  test_context      5/5
  test_integration  4/4
  test_io           7/7
  test_cascade      6/6
  test_canvas       6/6
  test_adaptive     8/8
  test_subtitle     8/8
  ───────────────────────
  total            69/69   ALL TESTS PASSED
```

### 밝기 분포

```
  "귀여운 고양이가 밥을 먹는다."    →  active 40, max A = 9, total 297
  conservation:                     297 = 40 + 185 + 72    (base+word+morph)
```

### 매칭 무결성

```
  block-skip vs full cosine               0.000% difference
  KF0↔KF1 cosine (similar clauses)       73.2%
  KF0↔KF2 cosine (different clauses)      0.0%
  self-query cosine                     100.0%
```

### 파이프라인 스모크 테스트 (`test_wiki data/sample_en.txt`, 50 clauses)

```
  clauses placed        50 / 50
  canvases              2
  self-query avg sim    100.0%
  cascade step 1 hits   50
  fallbacks             0
  Recall@1 / @5 / @10   100% / 100% / 100%
  next-clause top-1     22.0%     (beats best-of-5 random = 20%)
  save size             20.9 MB   (.spai on disk)
  load + append         OK        (4 canvases, 100 slots after append)
```

### `stream_train` 스모크 테스트 (1만 줄 코퍼스에서 1000절)

```
  ingest                clauses=1000  KF=49  Δ=951   elapsed 4.64 s
  .spai file size       17.2 MB
  verify tail (500)     avg sim 0.9867  min 0.34  max 1.000
  hits ≥ 0.90 / 0.50    490 / 490
  R range               0..210
  G range               0..120
  B range               0..200         (POS seed active, was 0..0)
```

### 통합 매칭 성능

`match_cascade`, `match_engine`, `ai_predict`가 모두 `spatial_match()`에 위임되고,
`BUCKET_THRESHOLD = 100` 이후 bucket index가 O(N) 스캔을 건너뛰면서,
동일 1만 줄 합성 코퍼스 기준 `stream_train` 시간이 **58.85 s → 22.14 s**
(−62%)로 감소했습니다.

### 캐스케이드 / 캔버스

```
  cascade early-return on exact clause     ≥ CASCADE_STEP1_THRESHOLD (0.5)
  ai_force_keyframe 1-1 mapping            kf_count == N, df_count == 0
  pool_match jumps to same-type slots      step=1 on matching DataType
  pool_match fallback to other types       step=4 when query type empty
```

---

## 빌드 & 실행

```bash
# 전체 빌드
make all

# 전체 테스트 (12개 바이너리, 69개 테스트)
make test

# 정리
make clean

# 위키 스타일 파이프라인 프로브 (data/sample_en.txt 또는 data/sample_ko.txt 사용)
make wiki
./build/test_wiki data/sample_en.txt
./build/test_wiki data/sample_en.txt --save build/model.spai
./build/test_wiki data/sample_en.txt --load build/model.spai
```

**요구 환경:** GCC (C11), Make, POSIX(`posix_memalign`) 또는 Windows MinGW.
시각화는 Python 3, `numpy`, `Pillow`, `ffmpeg`가 필요합니다.

### 스트리밍 트레이너 (`stream_train`)

대용량 코퍼스(전체를 RAM에 올리기 어려운 경우):

```bash
make stream
./build/stream_train --input data/kaggle_train.txt \
                     --max 25000 \
                     --save build/models/wiki25k.spai \
                     --checkpoint 5000 \
                     --verify
```

`stream_train`은 `fgets`(4 KB 버퍼)로 줄 단위 절을 읽고 절마다 `ai_store_auto`를 호출합니다.
입력 파일 크기와 무관하게 메모리 사용은 평탄하게 유지됩니다.
`--checkpoint N`은 N절마다 `foo.ckpt_NNNNNN.spai`를 출력하고,
`--verify`는 학습 말미를 재스캔해 avg/min/max 유사도와 0.9/0.5/0.1 hit 수를 보고합니다.

### 실전 E2E 테스트

빌드→학습→검증→시각화 경로를 한 번에 실행:

```bash
./tools/run_practical_test.sh              # 기본: 1000절
./tools/run_practical_test.sh 5000
./tools/run_practical_test.sh 25000 data/kaggle_train.txt
```

체크포인트 간격은 `min(N / 2, 5000)`이며,
코퍼스 경로를 생략하면 `data/sample_en.txt` / `data/sample_ko.txt`로 fallback합니다.

### 학습 진화 시각화

```bash
pip install numpy Pillow
python3 tools/visualize_training.py build/models
```

디렉터리 내 `.spai` 체크포인트마다 1280×720 PNG를 렌더링하고
(`A` 히트맵, RGB 합성, KF/Δ 통계, 적응 가중치, EMA 커버리지, 샘플 라벨),
`ffmpeg`로 프레임당 3초의 `training_evolution.mp4`를 생성합니다.
RGB 합성에서 B 채널 기여도까지 시각적으로 확인할 수 있습니다.

### 벤치마크(선택)

```bash
make bench        # bench_stsb / bench_perplexity / bench_word_predict / bench_qa 빌드

./build/bench_word_predict  data/sample_en.txt  1000
./build/bench_qa            data/qa.tsv
./build/bench_perplexity    data/sample_en.txt  500
./build/bench_stsb          data/stsb.tsv
```

`bench_word_predict`는 `--save`, `--load`, `--load-only`를 지원합니다.
`--save` 없이 실제 학습이 발생하면 `build/models/bench_word_predict_auto.spai`로 자동 저장됩니다.

### 빠른 코퍼스 부트스트랩 (위키피디아 abstract 3천 줄)

`enwiki-latest-abstract1.xml.gz`를 전체 다운로드하지 않고 일부만 스트리밍해
abstract 3000줄(짧은 줄 필터 후 약 2500 usable clause)을 추출합니다.
Kaggle/Linux에서 동작합니다.

```python
import os, gzip, re, urllib.request
os.makedirs("data", exist_ok=True)
URL  = "https://dumps.wikimedia.org/enwiki/latest/enwiki-latest-abstract1.xml.gz"
OUT  = "data/kaggle_train.txt"
pat, n = re.compile(rb"<abstract>([^<]+)</abstract>"), 0
with urllib.request.urlopen(URL) as resp, gzip.GzipFile(fileobj=resp) as gz, \
     open(OUT, "wb") as out:
    buf = b""
    while n < 3000:
        c = gz.read(1 << 20)
        if not c: break
        buf += c
        last = 0
        for m in pat.finditer(buf):
            s = m.group(1).strip()
            if len(s) >= 20:
                out.write(s + b"\n"); n += 1
                if n >= 3000: break
            last = m.end()
        buf = buf[last:]
print(f"{OUT}: {n} lines, {os.path.getsize(OUT)/1e6:.1f} MB")
```

이후:

```bash
./tools/run_practical_test.sh 2500 data/kaggle_train.txt
python3 tools/visualize_training.py build/models
```

---

## 저장 / 로드

바이너리 포맷은 `.spai`이며 magic은 `SPAI`, 현재 버전은 **5**입니다.
버전 3/4도 투명하게 로드되며 누락 필드는 0으로 기본 처리됩니다.

```
  [Header 32B]   magic "SPAI" | version | kf_count | df_count | reserved[3]

  [Records]*     tagged stream, KFs + deltas in insertion order
    tag 0x01  Keyframe:  id + label[64] + text_byte_count +
                         (v5: topic_hash + seq_in_topic) +
                         A + R + G + B
    tag 0x02  Delta:     id + parent_id + label[64] + count + change_ratio +
                         entries[]  (v4+: 9 B/entry with diff_B; v3: 8 B)
    tag 0x03  Weights:   global ChannelWeight (4× float)
    tag 0x04  Canvas:    slot_count, canvas_type, frame_type, parent_canvas_id,
                         changed_ratio, classified, SlotMeta[32], A + R + G + B
    tag 0x05  Subtitle:  count + (type, topic_hash, canvas_id, slot_id, byte_length)[]
    tag 0x06  EMA:       R[65536] + G[65536] + B[65536] + count[65536]  (float each, 1 MB)
```

버전 변경점:

- **v3 → v4**: `DeltaEntry`에 `diff_B` 추가(B 채널도 sparse delta에 포함), EMA 후행 블록 추가
- **v4 → v5**: 모든 키프레임에 `topic_hash + seq_in_topic` 추가
  (`ai_generate_next`가 `id+1` fallback 대신 같은 topic 스레드를 따라가도록 개선)

공개 API (`include/spatial_io.h`):

```c
SpaiStatus ai_save(const SpatialAI* ai, const char* path);
SpatialAI* ai_load(const char* path, SpaiStatus* out_status);
SpaiStatus ai_save_incremental(const SpatialAI* ai, const char* path);
SpaiStatus ai_peek_header(const char* path,
                          uint32_t* kf_count, uint32_t* df_count, uint32_t* version);
```

`test_io`로 검증한 보장:

- **라운드트립 무결성**: 700절 save → destroy → load 후 동일 질의 코사인 오차 `1e-3` 이내
- **증분 저장 성장 보장**: `ai_save_incremental`은 축소를 거부(`SPAI_ERR_STATE`)하고 신규 레코드만 추가
- **전방 호환성**: 알 수 없는 후행 태그를 허용하고 안전 종료
- **손상 파일 안전성**: bad magic → `SPAI_ERR_MAGIC`, bad version → `SPAI_ERR_VERSION`, truncated body → `SPAI_ERR_READ`

---

## 프로젝트 구조

```
├── include/                  # 공개 헤더
│   ├── spatial_grid.h        # 256×256 grid, 1D 정렬 채널
│   ├── spatial_layers.h      # 3-layer 합산 (base / morpheme / word)
│   ├── spatial_morpheme.h    # 한국어 형태소 분석기 (최장일치)
│   ├── spatial_keyframe.h    # 키프레임 / 델타 / SpatialAI 엔진
│   ├── spatial_match.h       # 코사인, 캐스케이드 모드, 적응 가중치
│   ├── spatial_context.h     # 컨텍스트 프레임 + LRU 캐시
│   ├── spatial_canvas.h      # 2048×1024 canvas (32 슬롯)
│   ├── spatial_subtitle.h    # SubtitleTrack + SpatialCanvasPool
│   ├── spatial_generate.h    # 다음 절 생성
│   └── spatial_io.h          # .spai 바이너리 포맷 (v3)
├── src/                      # 헤더별 구현
├── dict/                     # 한국어 사전 (명사/동사/형용사/조사/어미)
├── tests/                    # 12개 테스트 바이너리, 총 69개 테스트
├── data/                     # 샘플 코퍼스 + 다운로드 스크립트
├── tools/
│   ├── stream_train.c         # 줄 단위 스트리밍 학습기 (C 바이너리)
│   ├── run_practical_test.sh  # 빌드 + 학습 + 검증 래퍼
│   ├── visualize_training.py  # .spai → PNG 프레임 + MP4 비디오
│   └── kaggle_gpu_train.py    # 선택적 CUDA 학습 헬퍼
├── Makefile
├── SPEC.md                   # 코어 명세 (Page 1)
├── SPEC-ENGINE.md            # 엔진 최적화 명세 (Page 2)
└── README.md / README_KO.md
```

---

## 엔진 최적화

주요 최적화는 `src/spatial_match.c` + `src/spatial_keyframe.c`에 있으며,
`test_match`, `test_integration`, `test_cascade`, `test_adaptive`로 검증합니다.

| 최적화 | 위치 | 결과 |
|---|---|---|
| 1D 정렬 채널(32-byte) | `spatial_grid.c` | AVX2 준비, 캐시 친화 |
| 블록 스킵(16×16 sum) | `compute_block_sums`, `cosine_block_skip` | 절 기준 93% 블록 스킵, 정확도 손실 **0.000%** |
| Adaptive Top-K(hash bucket) | `bucket_index_*`, `grid_hash` | KF ≥ 100에서 O(N) → O(N/B + K) |
| Sparse delta | `compute_delta` / `DeltaEntry` | 16-entry delta = 128 B |
| LRU 프레임 캐시 | `spatial_context.c` | 반복 접근 90% hit |
| 적응 채널 가중치 | `ChannelWeight` + `weight_update` | 저장 시 winner-take-reward |
| 방향성 RGB 확산 | `update_rgb_directional` | read/write 분리, 최소 ±1 반영 |

목표 성능은 키프레임 1000+에서 순진한 셀 단위 코사인 대비
복합 20–100× 가속이며, 정확도는 유지합니다.

---

## 형태소 분석기

사전 기반 최장일치 토크나이저이며 외부 라이브러리 없이 동작합니다.

```
  입력              출력
  ─────────────────────────────────────────────────
  "귀여운"        → [adj: 귀여운]
  "고양이가"      → [noun: 고양이] + [particle: 가]
  "밥을"          → [noun: 밥] + [particle: 을]
  "먹는다."       → [verb: 먹] + [ending: 는다] + [punct: .]
```

```
  사전
  ├── Nouns       88   (동물, 음식, 사물, 자연, 사람, 추상)
  ├── Verbs       39   (먹, 가, 오, 보, 하, 되, …)
  ├── Adjectives  20   (귀여운, 예쁜, 밝은, 아름다운, …)
  ├── Particles   26   (은/는/이/가/을/를/에서/으로, …)
  └── Endings     20   (는다/었다/았다/겠다, …)
```

POS 태그는 방향 확산 전에 R/G 채널 초기 시드로도 반영됩니다.

```
  POS_NOUN     R=40  G=30
  POS_VERB     R=120 G=40
  POS_ADJ      R=170 G=35
  POS_PARTICLE R=8   G=85
  POS_ENDING   R=12  G=95
  POS_PUNCT    R=5   G=120
  POS_UNKNOWN  R=210 G=20
```

---

## 기존 LLM과 비교

```
                      Traditional LLM            SPATIAL-PATTERN-AI
  ───────────────────────────────────────────────────────────────────
  Encoding            token → vector → matrix    byte → pixel → 256×256
  Parameters          fixed-size matrix          unlimited frame stack
  Context             bounded window (32K-1M)    unlimited (disk-bound)
  Interpretability    opaque weights             visible heatmap
  Learning            full retrain / SFT         incremental delta
  Per-frame cost      —                          ~320 KB on disk
  Retrieval           attention / embed search   overlap → cosine → cascade
```

강점: 리트리벌, 증분 메모리, 되감기 가능한 학습, 해석 가능성, 임베디드 친화성.
트레이드오프: 생성형 LLM 대체제가 아니라,
패턴 지속성이 중요한 리트리벌/메모리 중심 작업의 기반 엔진입니다.

---

## 라이선스

[LICENSE](LICENSE)를 참고하세요.
