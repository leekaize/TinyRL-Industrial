# The TinyRL stack on ESP32-S3

Notes on the [TinyRL](https://github.com/mohmdelsayed/TinyRL) submodule ([docs](https://www.tinyrl.ai/docs/)) on the ESP32-S3 N16R8 (16 MB quad SPI flash + 8 MB octal PSRAM). From the upstream analysis of the submodule at commit `82f0774`; upstream files are cited inline.

The upstream library never stores weights in flash and never calls PSRAM APIs directly. This project enables both through `nodes/core/persistence.cpp`, which saves `net.bin` (weights) and `model.bin` (stats/theta) so a rebooted node resumes monitoring with zero relearning. That is what makes the node plug-and-play.

## Master architecture

![Master architecture](/assets/arch-master.png)

Five layers: (5) application sketches, (4) Stream-X RL module, (3) autograd core, (2) platform and build configuration, (1) ESP32-S3.

## Memory layout

TinyRL contains zero direct `heap_caps_malloc(MALLOC_CAP_SPIRAM)` calls. All memory flows through `std::vector<float>` inside `ag::Matrix` (`matrix.h:44`), so plain `operator new` handles allocation. PSRAM use is achieved entirely at build time: `CONFIG_SPIRAM_USE_MALLOC=1` makes the Arduino-ESP32 heap allocator place large blocks (weights, eligibility traces, frame buffers) into the 8 MB octal PSRAM, while small hot objects stay in internal SRAM served by the 32 KB L1 cache.

Enablers in `platformio.ini` and the board JSON:

```ini
-DBOARD_HAS_PSRAM
-DCONFIG_SPIRAM_USE_MALLOC=1        ; transparent PSRAM for big allocations
-mfix-esp32-psram-cache-issue       ; S3 PSRAM cache errata workaround
board_build.arduino.memory_type = dio_opi   ; octal (OPI) PSRAM + DIO flash
```

Fragmentation-proof frame storage (upstream `stream_q_atari.cpp`): fixed-size `MAX_FRAMES = 4` array instead of `std::deque`, plus reusable reshape and stack buffers.

## Compute layout

An honest technical note from the upstream analysis: the ESP32-S3 LX7 cores expose a 128-bit SIMD vector ISA, and TinyRL does not call any Xtensa SIMD intrinsics or esp-dsp functions. `esp32_dsp.h` ships hand-tuned pure scalar kernels whose 2-4x speedup comes from:

1. 4/8-way loop unrolling, feeding the LX7's dual-issue pipeline,
2. cache-blocked GEMM with `BLOCK = 32`, sized for the 32 KB L1 cache,
3. `-O3 -ffast-math`, letting GCC auto-vectorize the straight-line loops,
4. stride-specialized im2col with a memcpy fast path for stride 1.

Dispatch is compile-time (`src/simd.h`): `AG_PLATFORM_ESP32` selects the ESP32-S3 kernels, NEON the ARM64 ones, everything else falls back to a generic BLOCK=64 path. The optimizer update and the ObGD eligibility-trace step (`trace_update_l1`, `fma_scalar`) run through the same kernels. The overshooting-bounded adaptive step size (kappa bound) is computed from the traces' L1 norm (`obgd.h`).

## Flash and storage

Upstream: neural-network weights are never read from flash in the library's own examples. They are random-initialized at boot from the hardware TRNG and learned online; flash holds only the firmware image, because the header-only design compiles the framework, agent and environment into one binary.

This project extends that: `persistence.cpp` adds the four node files (model.bin, net.bin, conditions.jsonl, pending.jsonl) on LittleFS. The build keeps binaries small with `-O3 -ffunction-sections -fdata-sections` plus linker GC, and `AG_EMBEDDED` switches logging to a minimal serial path.

## Runtime services

- **Hardware TRNG bridge**: a `getentropy` shim over `esp_random()` seeds the C++ RNG (`esp_shim.c` in our tree, `getentropy_dummy.c` upstream).
- **Kernel benchmarking**: `benchmark_kernels.cpp` times matmul with `micros()`.
- **Power management** (optional, documented upstream): `esp_pm_configure` downclocks to 80 MHz with light sleep.
- **Telemetry**: USB-CDC/UART at 115200 Bd.

## Summary of hardware-utilization mechanisms

| ESP32-S3 capability | How TinyRL uses it | Where in code |
|---|---|---|
| Octal PSRAM (8 MB) | transparent spill of large allocations via `CONFIG_SPIRAM_USE_MALLOC=1`; no explicit `heap_caps` calls | `platformio.ini`, `matrix.h:44` |
| Internal SRAM 512 KB + 32 KB L1 | small/hot objects; GEMM blocking `BLOCK=32` sized to cache | `esp32_dsp.h:276-278` |
| Flash | firmware only upstream; our node adds the four persistence files | board JSON `default_8MB.csv`, `persistence.cpp` |
| PSRAM cache errata | compiler workaround flag keeps PSRAM cache coherent | `-mfix-esp32-psram-cache-issue` |
| LX7 dual-core 240 MHz + FPU | hand-tuned scalar kernels: unrolling, fused FMA sweeps; no SIMD intrinsics | `esp32_dsp.h`, `simd.h` |
| Hardware TRNG | seeds the C++ RNG through the `getentropy` shim | `esp_shim.c` |
| USB-CDC / UART | 115200 Bd telemetry | `stream_ac_continuous.cpp:71` |
| esp_timer / micros | kernel-level benchmarking | `benchmark_kernels.cpp` |
| Watchdog / RTOS | `delay(1)` yields, 16 KB loopTask stack | `platformio.ini` |
| WiFi + OTA | optional telemetry and firmware updates (documented, not in core sketches) | ESP32 guide |

## Verbatim excerpts (upstream sources)

The RL step, from `stream_ac_continuous.cpp`:

```cpp
void loop() {                                        // 1 full RL step per iteration
    ag::Tensor s(state, false);                      // state tensor, no grad
    ag::Tensor action = agent.sample_action(s);      // actor forward + sample
    auto [next_state, reward, terminated, truncated] = env.step(action.item());
    next_state = agent.normalize_observation(next_state);
    float scaled = agent.scale_reward(reward, terminated || truncated);
    ag::Tensor r(ag::Matrix::Constant(1, 1, scaled), false);
    ag::Tensor sn(next_state, false);
    agent.update(s, action, r, sn, terminated || truncated);  // TD error -> ObGD
}
```

The single storage primitive, `src/matrix.h:44` (every tensor, weight, trace and frame ends up here):

```cpp
class Matrix {
public:
    using Shape = std::array<int, 4>;  // {N, C, H, W}
private:
    Shape shape_{1, 1, 1, 1};
    std::vector<Float> data_;          // <- all memory goes through new/delete
```

Board definition, `boards/freenove_esp32_s3_wroom.json`:

```json
"build": {
  "arduino": { "ldscript": "esp32s3_out.ld",
               "partitions": "default_8MB.csv",
               "memory_type": "dio_opi" },
  "extra_flags": ["-DBOARD_HAS_PSRAM", "-DESP32S3_DEV",
                  "-mfix-esp32-psram-cache-issue"],
  "f_flash": "80000000L", "flash_mode": "dio", "psram_type": "opi"
},
"upload": { "flash_size": "8MB", "maximum_ram_size": 327680,
            "maximum_size": 8388608 }
```

Fragmentation-proof frame storage, `stream_q_atari.cpp`:

```cpp
// Fixed-size array instead of std::deque  (avoid heap fragmentation on ESP32)
static constexpr int MAX_FRAMES = 4;
ag::Matrix frames_[MAX_FRAMES];   // pre-allocated 42x42 grayscale frames
ag::Matrix frame_hw_;             // reusable reshape buffer
ag::Matrix out_;                  // reusable stack buffer (1, n, h, w)
```

Runtime memory monitoring (ESP32 guide):

```cpp
void print_memory_info() {
    Serial.printf("Free heap: %d\n", esp_get_free_heap_size());
    Serial.printf("Largest free block: %d\n", esp_get_max_alloc_heap());
    Serial.printf("Minimum free heap: %d\n", esp_get_minimum_free_heap_size());
}
```

Platform detection + dispatch, `src/simd.h:55-58, 453-469`:

```cpp
// ESP32-S3 Detection (checked first - most constrained platform)
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV) || \
    defined(ARDUINO_FREENOVE_ESP32_S3_WROOM) || defined(ESP32S3)
  #define AG_PLATFORM_ESP32 1
#endif

AG_INLINE void matmul(const Float* A, const Float* B, Float* C, int M, int K, int N) {
#if defined(AG_PLATFORM_ESP32)
    matmul_esp32(A, B, C, M, K, N);   // <- ESP32-S3 kernel
#elif defined(AG_PLATFORM_NEON)
    matmul_neon(A, B, C, M, K, N);    // <- Apple-Silicon / ARM64
#else
    ...                               // generic BLOCK=64 fallback
#endif
}
```

Cache-blocked GEMM, `src/esp32_dsp.h:276-296`:

```cpp
AG_ESP32_INLINE void matmul_esp32(const Float* A, const Float* B, Float* C,
                                  int M, int K, int N) {
    std::memset(C, 0, M * N * sizeof(Float));
    constexpr int BLOCK = 32;  // Optimized for ESP32 cache (32 KB L1)

    for (int i = 0; i < M; i += BLOCK)
      for (int k = 0; k < K; k += BLOCK)
        for (int j = 0; j < N; j += BLOCK) {
            int imax = std::min(i + BLOCK, M);   // ...
            for (int ii = i; ii < imax; ++ii) {
                for (int kk = 0; kk < kmax; ++kk) {
                    Float aik = A[ii * K + kk];
                    fma_scalar(B + kk * N + j, aik, C + ii * N + j, jmax - j);
                }   // whole inner row = one fused multiply-add sweep
            }
        }
}
```

Instruction-level parallelism: 4-way unrolled dot product, `esp32_dsp.h:158-168`:

```cpp
template<> AG_ESP32_INLINE Float dot<Float>(const Float* a, const Float* b, size_t n) {
    Float s0 = 0, s1 = 0, s2 = 0, s3 = 0;        // 4 independent accumulators
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i]*b[i];     s1 += a[i+1]*b[i+1];
        s2 += a[i+2]*b[i+2]; s3 += a[i+3]*b[i+3];   // ILP on dual-issue LX7
    }
    Float s = (s0 + s1) + (s2 + s3);             // reduction tree
    for (; i < n; ++i) s += a[i] * b[i];         // tail
    return s;
}
```

Fused optimizer kernel, `src/optimizer.h:53-62` (`AG_ESP32_DSP` path):

```cpp
void step() override {
    for (auto* p : params_) {
        if (p && p->requires_grad()) {
#if defined(AG_ESP32_DSP)
            // Use ESP32-S3 optimized SGD update (fused, in-place, unrolled)
            simd::sgd_update(p->value().data(), p->grad().data(),
                             lr_, p->value().size());
#else
            p->set_value(p->value() - p->grad() * lr_);   // generic: extra copies
#endif
        }
    }
}
```

Streaming-RL eligibility traces, `obgd.h step()` + `simd.h:432`:

```cpp
// 1. Update traces & compute L1 norm using SIMD kernel
for (size_t i = 0; i < n_params; ++i) {
    ag::Float* trace_data = etraces_[i].data();
    const ag::Float* grad_data = parameters_[i]->grad().data();
    z_sum += ag::simd::trace_update_l1(trace_data, grad_data, gamma_lambda_, size);
}
// 2. Overshooting-bounded adaptive step size (kappa)
const ag::Float bound = delta_bar * z_sum * lr_kappa_;
const ag::Float step_size = (bound > ag::Float(1)) ? lr_ / bound : lr_;
// 3. Parameter update: p += -step_size * delta * e_trace
for (size_t i = 0; i < n_params; ++i)
    ag::simd::fma_scalar(trace_data, neg_update_scale, param_data, size);
```

Size-conscious build, `platformio.ini` / ESP32 guide:

```ini
-O3
-ffunction-sections
-fdata-sections        ; + linker GC lets the binary keep only used kernels
```

```cpp
// AG_EMBEDDED switches logging to a no-op/serial minimal path (src/log.h)
#define AG_EMBEDDED 1
```

Hardware TRNG bridge, `getentropy_dummy.c` (entire file; ours is `nodes/core/esp_shim.c`):

```c
#include <stdint.h>
#include <stddef.h>
#include "esp_random.h"

int getentropy(void *buf, size_t buflen) {
    uint8_t *buffer = (uint8_t *)buf;
    for (size_t i = 0; i < buflen; i++) {
        buffer[i] = (uint8_t)(esp_random() & 0xFF);   // hardware true RNG
    }
    return 0;
}
```

Kernel benchmarking, `benchmark_kernels.cpp`:

```cpp
ag::Matrix A = ag::Matrix::Random(1, INPUT_DIM);
ag::Matrix B = ag::Matrix::Random(INPUT_DIM, HIDDEN_DIM);

unsigned long start = micros();                      // esp_timer under the hood
for (int i = 0; i < NUM_ITERATIONS; i++) {
    ag::Matrix C = A.matmul(B);                      // -> matmul_esp32()
    volatile float x = C(0, 0);                      // prevent optimization
}
Serial.printf("MatMul: %lu us per op\n",
              (micros() - start) / NUM_ITERATIONS);
```

Optional power management (ESP32 guide):

```cpp
esp_pm_config_esp32s3_t pm_config = {
    .max_freq_mhz = 80,          // downclock from 240 MHz
    .min_freq_mhz = 10,
    .light_sleep_enable = true
};
esp_pm_configure(&pm_config);
```
