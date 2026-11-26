# DGC optimization plan (Turnip / A740)

Date: 2026-11-26. Context: local `mesa27-star`, probe-only (no game runs). Mesa `dgc` branch local commits include `4c41304cfb2`, `78d4cb8750d`, `1d7c83646a3`, `5636aa45f14`, `17be3c1544c`.

## DGC

1. **Fix explicit preprocess / `isPreprocessed`** — done locally.
   - Fixed BO iova registration/lookup for Virtio/MSM/KGSL paths and made iova insert/remove idempotent.
   - Preprocessed execute now validates preprocess BO range and emits application draw/compute state before chaining the preprocessed PM4 stream.
   - Preprocess-only recording outside a render pass no longer emits draw state; state is emitted on preprocessed execute.
   - Smoke: `dgc/tests/dgc-probe/probe20.c` passes (`explicit/unpre`, `explicit/pre`).

2. **Measure preprocess benefit** — initial probe data collected.
   - `probe21` (explicit-preprocess layout, A740, tiny triangle, N=512 median): `draw` unpre 874us total / pre 380us; `pc_draw` unpre 1823us / pre 903us; `indexed` unpre 2042us / pre 419us. Manual CPU loop remains lower total at N=512 in this tiny GPU-idle case (`draw` 194us, `pc_draw` 890us, `indexed` 213us).
   - Interpretation: preprocessed execute removes much DGC overhead, but CPU-recorded manual loop is still competitive/faster for tiny draws; PC-heavy case benefits most and approaches manual total at N=512. Needs confirmation with larger GPU workload/fat mode and real token mixes.

3. **Indexed+IB DGC fast path** — partially done.
   - Batching by identical IB/size/format is implemented. `sequenceCountAddress` is now rejected by the CPU-read IB fast path and falls back to generic DGC path (correctness over speed).
   - TODO: support `sequenceCountAddress` safely in fast path, validate differing IB/stride/offset/limits, reduce per-run CPU overhead/state checks.

4. **PC-heavy/generic path** — pending profile after p.2/p.3.
   - Main expected win is reuse of preprocessed buffer. Consider WFI/dispatch overhead reduction only with profile evidence and const-state correctness checks.

5. **VERTEX_BUFFER / Starfield** — after probe baseline and safe token-mix capture.
   - `VERTEX_BUFFER` still not implemented; Starfield token mix unknown (no game runs now).

## Outside DGC (only after profile)

- **Flush/barrier cost**: investigate extra `WAIT_FOR_IDLE/WAIT_FOR_ME` only with traces/counter-tests.
- **Hot-path allocations**: replace `tu_cmd_render_tiles` subsampled apron malloc/free with scratch/arena if active.
- **BO alloc/map churn**: audit command/query/descriptor paths before adding pools/suballoc.
- **iova->BO lookup contention**: `tu_device_lookup_bo_by_iova` uses global `bo_mutex`; acceptable for rare DGC lookups, avoid broadening without read-mostly design.

## Validation rules

- Build only in podman: `SRC=/var/home/armada/opencode/mesa27-star OUT=/tmp/opencode/mesa27-star-build HOSTOUT=/var/home/armada/opencode/build/out-star FRESH=0 ./dgc/scripts/build-dgc27-host.sh`.
- Probes: `VK_DRIVER_FILES=/var/home/armada/opencode/build/out-star/freedreno_icd.json ./probe19` and targeted probes (`probe20`, `probe21`).
- No game runs, no `DRIRC_CONFIGDIR`, do not modify dirty `mesa27-src`.
