# A740 DGC target architecture

Target: Adreno A740 (`a740_sqe.fw`), Turnip/VKD3D-Proton DGC. This is the working architecture to optimize for; non-essential alternatives are out of scope unless required for correctness.

## 1. No nesting/branching in IB2 (strict IB1 -> IB2)

- IB2 is a strictly flat linear PM4 stream: no IB3, no trampolines, no `CP_INDIRECT_BUFFER_CHAIN`, no `CP_COND_EXEC`. This avoids disturbing ROQ prefetch FIFO and `IB2_REM_DWORDS` return counting.
- If the buffer exceeds the 20-bit limit (`0xFFFFF` DWORDs), IB1 issues several flat IB2 chunks in sequence.

## 2. Fast paths by workload

- **Draw-only (no state changes):** hardware `CP_DRAW_INDIRECT_MULTI` directly over the game's buffer (0 bytes PM4 generation + hardware VSC tile culling).
- **General DGC (PC/SI/IB/state):** GPU compute preprocess shader writes a compact linear stream with vector `uvec4` stores, local/global compaction via `subgroupExclusiveAdd`, and duplicate-state dedup via `subgroupShuffleUp`; empty slots are filled with `CP_NOP`.

## 3. Long NOP-tail trimming (no CP_COND_EXEC, no size=0)

- Reserve a one-shot/per-submit versioned 16-byte slot in IB1 before dispatch, initialized as a safe 4-DWORD `CP_NOP(cnt=3)`.
- After `UCHE_INVALIDATE + CP_WAIT_MEM_WRITES + WFI` and a new IB1 chunk (ROQ prefetch protection), preprocess overwrites the slot with one `uvec4` as `CP_INDIRECT_BUFFER(..., active_size & 0xFFFFF)` only when `active_size > 0`. If `active_size == 0`, slot remains `CP_NOP` and IB2 is not executed.
- Never emit `CP_INDIRECT_BUFFER` with `size == 0`. `ib2 .../0000` in devcoredump indicates SQE executed size=0 and is a fatal IB1 parser desync symptom on A740.

## 4. Tile rendering accounting (GMEM vs SYSMEM)

- Static pipeline state (on shader changes) goes into baked `CP_SET_DRAW_STATE` (SDS) blocks split into BINNING and GMEM_ONLY.
- For heavy general DGC in a render pass with frequent state changes (HLSQ), prefer SYSMEM over GMEM (1 pass instead of ~30 tiles).

## Immediate correctness constraints

- Slot patch visibility requires barrier ordering and ROQ prefetch distance/chunk boundary; placing the slot a few DWORDs after WFI in the same prefetched IB1 region is unsafe.
- A7xx-safe dynamic executed DGC: no CPU CS patching, no `CP_INDIRECT_BUFFER_CHAIN` in generated/IB2 execution path; IB1 control flow must remain simple and prefetch-safe.
