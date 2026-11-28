# A740 DGC target architecture

Target: Adreno A740 (`a740_sqe.fw`), Turnip/VKD3D-Proton DGC. This is the working architecture to optimize for; non-essential alternatives are out of scope unless required for correctness.

## 1. Execution model (strict IB1 -> IB2)

- **No nesting/branching in IB2.** IB2 is a strictly flat linear PM4 stream: no IB3, no trampolines, no `CP_INDIRECT_BUFFER_CHAIN`, no `CP_COND_EXEC`. This avoids disturbing ROQ prefetch FIFO and `IB2_REM_DWORDS` return counting.
- **20-bit chunking.** If the generated buffer exceeds `0xFFFFF` DWORDs, IB1 issues several flat IB2 chunks in sequence.
- **A7xx correctness constraints.** No CPU CS patching. No `CP_INDIRECT_BUFFER_CHAIN` in generated/IB2 execution path. IB1 control flow must remain simple and prefetch-safe.

## 2. Fast paths by workload

### 2.1 Draw-only (no state changes)

- Use hardware `CP_DRAW_INDIRECT_MULTI` directly over the game's indirect buffer.
- No PM4 generation, no preprocess compute for this path. Hardware VSC/tile culling is preserved.
- Applies to fixed-count and indirect-count draw-only layouts where tokens do not mutate pipeline/state.
- Large fixed-count draw-only DGC is split into limit-sized native `CP_DRAW_INDIRECT_MULTI` packets directly over the game buffer.

### 2.2 General DGC (PC/SI/IB/state)

- GPU compute preprocess writes a compact linear PM4 stream.
- Use vector `uvec4` stores for token/slot patching.
- Use local/global compaction via `subgroupExclusiveAdd`.
- Fill inactive/empty slots with `CP_NOP`.
- **Deferred:** duplicate-state dedup via `subgroupShuffleUp` is *not* implemented. It may be useful for state-only sequences, but is not confirmed needed/correct/profitable on A740; do not add it until proven.

## 3. Slot/size safety (no CP_COND_EXEC, no size=0)

- Reserve a one-shot/per-submit versioned 16-byte slot in IB1 before dispatch.
- Initialize slot as a safe 4-DWORD `CP_NOP(cnt=3)`.
- After `UCHE_INVALIDATE + CP_WAIT_MEM_WRITES + WFI`, and at a new IB1 chunk / ROQ-prefetch-safe boundary, preprocess overwrites the slot with one `uvec4` as `CP_INDIRECT_BUFFER(..., active_size & 0xFFFFF)` only when `active_size > 0`.
- If `active_size == 0`, leave slot as `CP_NOP`; do not execute IB2.
- **Never emit `CP_INDIRECT_BUFFER` with `size == 0`.** `ib2 .../0000` in devcoredump indicates SQE executed size=0 and is a fatal IB1 parser desync symptom on A740.
- Slot patch visibility requires barrier ordering and ROQ prefetch distance/chunk boundary; placing the slot a few DWORDs after WFI in the same prefetched IB1 region is unsafe.

## 4. Tile rendering accounting (GMEM vs SYSMEM)

- Static pipeline state (on shader changes) goes into baked `CP_SET_DRAW_STATE` (SDS) blocks split into `BINNING` and `GMEM_ONLY` where applicable.
- For heavy general DGC in a render pass with frequent state changes (HLSQ), prefer `SYSMEM` over `GMEM` (1 pass instead of many tiles). This is a conservative performance/safety preference, not a blanket rule for draw-only DGC.

## 5. Current implementation status (2026-11-28)

- **Implemented in Mesa/Turnip (`mesa27-star`, branch `dgc`, local commit `a11991422a3`):** flat `IB1->IB2`, `>0xFFFFF` chunking, draw-only `CP_DRAW_INDIRECT_MULTI` fast path, general compact linear PM4 with `uvec4` token load/slot patch, contiguous active prefix, inactive-slot `CP_NOP` fill, one-shot 16B slots after `CCU/CACHE clean+invalidate + CP_WAIT_MEM_WRITES + WFI`, patch only when `active_size > 0`, never `size==0`, conservative `SYSMEM` hint for heavy general DGC.
- **Not implemented/deferred:** `subgroupShuffleUp` state dedup, detailed SDS BINNING/GMEM_ONLY split policy validation, ROQ/prefetch distance proof, devcoredump/probe26/27 hardware validation matrix.
- **Driver build:** `build/out-star/libvulkan_freedreno.so` sha256 `5c1080bd57541439eaf655ec798851a8f12694f9d3f0fce48c39a5d6a158b0c4`.

## 6. Teardown / vkd3d-proton validation status

- **vkd3d-proton raw VA fix:** local `19a0b8c3` in `vkd3d-proton` forces raw VA root CBVs when `EXT_dgc` is enabled; diagnostic `46018b65` adds `raw_va_mask`/bindless/type to DGC command-signature error. Both pushed to `github.com:keks2293/vkd3d-proton.git master`.
- **arm64 Proton-CachyOS/FEX:** x64 game loads d3d12/d3d12core as **ARM64 PE** from `prefix/drive_c/windows/system32`. Proton copies `files/lib/wine/vkd3d-proton/<host_pe_arch>/` to system32 and `<wow64_pe_arch>/` to syswow64; host is `aarch64-windows`.
- **Override delivery:** do not copy DLLs into game dirs. `dgc/scripts/run-game.sh` uses a private Proton copy and restores prefix DLL backups on cleanup. It must replace only matching PE-arch slots; arm64x (`coff-arm64x`) is valid for `aarch64-windows`, not `x86_64-windows`.
- **Observed:** plain llvm-mingw ARM64 and llvm-mingw ARM64X builds both caused early `C0000005 @ 7FFFFFB50100` before normal vkd3d init when used as full replacement. Using native Proton `d3d12.dll` + our ARM64X `d3d12core.dll` loaded vkd3d-proton `19a0b8c375df6c8+` and enabled EXT_dgc fast path; raw-VA signature error disappeared in latest run. User reported menu appeared, but logs still show early `C0000005`s and no `dxgi_vk_swap_chain_init/Present` in that capture - **DGC not yet proven working/correct in Teardown**.
- **Next validation:** get a clean Teardown run with visible swapchain/present/FPS, no C0000005 storm, then measure DGC behavior/perf on A740.
