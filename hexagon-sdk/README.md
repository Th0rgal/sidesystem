# Hexagon V79 NPU Bridge for NixOS (`SM8750` / Snapdragon 8 Elite)

`sidesystem-ctl` exposes the Snapdragon 8 Elite Compute DSP (**Hexagon V79**, `32300000.remoteproc-cdsp`) and zero-copy DMA-Heap allocator directly inside the NixOS container:

| Device Node | Major:Minor | Purpose |
| :--- | :---: | :--- |
| `/dev/fastrpc-cdsp` | `10:106` | Non-secure FastRPC channel to Hexagon V79 CDSP (supports Unsigned PD) |
| `/dev/fastrpc-cdsp-secure` | `10:107` | Secure FastRPC channel (reserved for signed QNN/DSP domains) |
| `/dev/dma_heap/system` | `253:1` | Zero-copy system DMA-BUF heap (shared across Oryon CPU, Adreno 830, Hexagon V79) |
| `/dev/dma_heap/qcom,system` | `253:3` | Qualcomm cached system DMA-BUF heap (optimal for Hexagon VTCM DMA transfers) |

## Verified Silicon Capabilities (Motorola Razr 60 Ultra, `SM8750`)

Verified directly from unprivileged NixOS (`thomas`) via [`hexagon-probe.c`](./hexagon-probe.c):

- **Architecture:** `ARCH_VER = 0x8c79` (**Hexagon V79**)
- **Vector Engine:** `HVX_SUPPORT_128B = 6` (**6× 1024-bit HVX SIMD units**)
- **On-Die Scratchpad:** `VTCM_PAGE = 0x800000` (**8 MiB VTCM SRAM**)
- **User Compute:** `UNSIGNED_PD_SUPPORT = 1` (custom unsigned Hexagon `.so` kernels can run without Qualcomm signing)
- **Async Pipeline:** `ASYNC_FASTRPC_SUPPORT = 1` (overlapped CPU/NPU submission)
- **Zero-Copy Memory:** `DMA_HEAP_IOCTL_ALLOC` + `mmap()` verified on `/dev/dma_heap/system`

## Quick Verification Inside NixOS

```bash
gcc -O2 -Wall hexagon-probe.c -o hexagon-probe
./hexagon-probe
```

Expected output:
```text
fastrpc-cdsp: opened (fd=3)
  ARCH_VER            = 0x8c79 (V79)
  HVX_SUPPORT_128B    = 6
  VTCM_PAGE           = 0x800000 (8 MiB)
  UNSIGNED_PD_SUPPORT = 1
  ASYNC_FASTRPC       = 1
dma_heap/system: allocated 4 MiB DMA-BUF (fd=4), mmap OK
```

## On-Device QNN HTP V79 Runtime & Next Steps

Because NixOS runs glibc while Android's `/vendor/lib64/libQnnHtp.so` and `libcdsprpc.so` are bionic-linked, there are two clean paths to run AI workloads (LLMs, Whisper, Stable Diffusion, OpenCV/HVX filters) on the Hexagon V79 NPU:

1. **Native NixOS FastRPC + Unsigned PD (pure glibc, zero Android dependency):**
   - Speak `FASTRPC_IOCTL_INVOKE` / `FASTRPC_IOCTL_INIT_CREATE` directly against `/dev/fastrpc-cdsp` + `/dev/dma_heap/qcom,system` (same kernel ABI exercised by `hexagon-probe.c`).
   - Compile custom C/C++ HVX kernels with upstream LLVM (`clang --target=hexagon-unknown-elf -mv79 -mhvx`) and load them into an Unsigned PD via `/vendor/dsp/cdsp/fastrpc_shell_unsigned_3`.
2. **QNN HTP V79 Server Bridge (`llama.cpp` / `ggml-rpc` / Whisper):**
   - Run a thin Android-side bionic service using the on-device Qualcomm runtime (`/vendor/lib64/libQnnHtp.so`, `/vendor/lib64/libQnnHtpV79Stub.so`, `/vendor/lib/rfsa/adsp/libQnnHtpV79Skel.so`) exposed over a local Unix socket or `127.0.0.1` RPC port to NixOS clients (`llama-cli --rpc`, Python, or `omarchnix` voice/AI tools).

> **Can Hexagon V79 accelerate desktop rendering?**
> Not directly for OpenGL/Vulkan rasterization (Adreno 830 v2 already hits `585.5 FPS` at `2560×1440` with 12 MiB GMEM and `2–5%` idle CPU). Hexagon V79 is ideal for **AI & signal workloads**: local LLM inference (45+ TOPS INT4/INT8), real-time Whisper transcription (`murmure`), neural upscaling / denoising, and camera/audio DSP pipelines.
