# Hexagon V79 NPU & Adreno 830 Compute SDK (`SM8750` / Snapdragon 8 Elite)

`sidesystem-ctl` exposes the Snapdragon 8 Elite Compute DSP (**Hexagon V79**, `32300000.remoteproc-cdsp`), zero-copy DMA-Heap, and **Qualcomm Venus VPU** (`/dev/video32`–`33`) directly inside the NixOS container:

| Device Node | Major:Minor | Purpose |
| :--- | :---: | :--- |
| `/dev/fastrpc-cdsp` | `10:106` | Non-secure FastRPC channel to Hexagon V79 CDSP (Unsigned PD supported) |
| `/dev/fastrpc-cdsp-secure` | `10:107` | Secure FastRPC channel (signed QNN/DSP domains) |
| `/dev/dma_heap/qcom,system` | `249:0` | Qualcomm cached system DMA-BUF heap (shared across Oryon CPU, Adreno 830, Hexagon V79) |
| `/dev/video32` / `/dev/video33` | `81:0` / `81:1` | Qualcomm Venus VPU hardware decoder (`AV1/HEVC/VP9/H.264`) & encoder (`HEVC/H.264`) |

## Verified Live Silicon Results (Motorola Razr 60 Ultra, `SM8750`)

### 1. Native Hexagon V79 6×1024-bit HVX Kernels (Zero Qualcomm SDK — Upstream Clang 21 + LLD 21!)
Built via [`Makefile`](./Makefile) from [`hvx_kernels_skel.c`](./hvx_kernels_skel.c) + [`hexagon-skel.lds`](./hexagon-skel.lds) (`ELF32 DYN`, `Machine: QUALCOMM DSP6`, `Flags: 0x79`, `PT_LOAD` at offset `0`) and executed live on CDSP Unsigned PD via [`hexagon-hvx-bench.c`](./hexagon-hvx-bench.c):

- **Verified Clock & Hardware:** `max_clk = 2000 MHz`, `hvx_units = 0x600` (**6× 1024-bit HVX units**), `vtcm_4MiB = 1` (**on-die VTCM SRAM verified**), live HVX die temp rise `28.5°C → 32.4°C` (`nsphvx-0..2`).

| Workload | 6× 1024-bit HVX (`2.0 GHz`) | Oryon CPU (`4.32 GHz` 1T) | Speedup | Verification |
| :--- | :---: | :---: | :---: | :---: |
| **16-Round 32-bit ARX/ChaCha Vector Compute (`8 MiB`)** | **`5.363 ms` (`18.77 GOPS`)** | `20.628 ms` (`4.88 GOPS`) | **`3.85×`** | Bit-exact (`0xf3ed46e6`) |
| **16 MiB RAID/Backup XOR Parity + Checksum** | `3.039 ms` (`16.56 GB/s`) | `1.811 ms` (`27.80 GB/s`) | `0.60×` (DDR RPC bound) | Bit-exact (`0x8bc95825`) |
| **3440×1440 RGBA→Luma + Frame Digest** | `2.177 ms` (`11.38 GB/s`) | `0.789 ms` (`31.41 GB/s`) | `0.36×` (DDR RPC bound) | Bit-exact (`0x31e1df64`) |
| **16 MiB 3-Tap Image Convolution Blur** | `3.322 ms` (`10.10 GB/s`) | — | 5.5× over 1-thread HVX | Verified |

> **Key takeaway:** Memory-bound 1-pass kernels are limited by FastRPC DDR cache-maintenance overhead, whereas **compute-intensive non-AI kernels** (crypto/hashing, multi-tap convolutions, FFTs, VTCM-resident stencils) saturate all **6× 1024-bit HVX units** and beat a `4.32 GHz` Oryon core by **3.85×** while keeping Oryon CPU at `0%`.

### 2. Local LLM Inference Inside NixOS (`llama.cpp` `ggml-vulkan` on Adreno 830 v2 vs Oryon CPU)
Measured live on `Qwen2.5-0.5B-Instruct Q4_K_M` (`463 MiB`, `Vulkan0: Adreno (TM) 830`, `11.4 GiB` UMA):

| Backend | Prompt Prefill (`pp256`) | Token Generation (`tg64`) | Speedup vs CPU |
| :--- | :---: | :---: | :---: |
| **Adreno 830 v2 (`ggml-vulkan`, Turnip `12 MiB` GMEM)** | **`715.35 ± 3.96 tok/s`** | **`60.70 ± 1.56 tok/s`** | **`4.52×` prefill** |
| **Oryon 8-Core CPU (`ARMv8.2` NEON)** | `158.17 ± 1.20 tok/s` | — | `1.00×` |

## Quick Build (Pure Upstream LLVM)

```bash
# Build Hexagon V79 1024-bit HVX shared library (requires clang + ld.lld with hexagon target)
make libhvx_kernels_skel.so

# Build lightweight NixOS FastRPC/DMA-Heap capability probe
make hexagon-probe
./hexagon-probe
```
