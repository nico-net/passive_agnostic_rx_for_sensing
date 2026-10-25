# DGX GB10: GPU ISA twin "dft_twin=none", root cause and fix (2026-10-25)

Branch `fix/dgx-isa-twin` from `origin/adaptive-rx-UL-DL` @ `c300718765`. Host: DGX Spark GB10 (aarch64, sm_121),
CUDA 13.0.88. Build: own build dir `cmake_targets/ran_build/build` (GPU on, `LDPC_CUDA_ARCH=89;121`).

## Symptom (reproduced)

`test_nr_gpu_isa_twin`: `GPU_ISA_TWIN dft_twin=none rot_twin=none (... dft=none rot=qrdmx ...)` -> FAIL, so the exact
GPU working set was refused and `test_nr_gpu_pass*` and `test_nr_pdsch_gpu_exact_size_4096` failed
[OFFLINE VERIFIED, DGX GB10 aarch64 unit, reference build and own build before the fix].
`rot_twin=none` is only how the log line is printed: it shows `none` for both when the profile is P_NONE. The rotation
self-test itself passed (`rot=qrdmx`) [CODE-READ `isa_select`, nr_pdsch_gpu_exact_fep.cuh].

## Root causes (two independent defects, plus test-harness gaps)

1. **The self-test's CPU reference ran on zero twiddles (aarch64 only).** In `nr_gpu_isa_ref.c`, when the process has no
   `dft()` pointer (path 2, e.g. the unit test), the code dlopens `libdfts.so` but never calls `dfts_autoinit()`.
   OAI's `load_module_shlib()` always calls it. `oai_dfts_neon.c` fills `tw1024/tw4096/...` only in `dfts_autoinit`
   (it has no constructor). `oai_dfts.c` (x86) has a constructor and lazy split-radix twiddles, and no `dfts_autoinit`.
   Evidence: before the fix, all 14 vectors had 4096/4096 outputs wrong, with a scale error of about 8x. After calling
   autoinit, the low-amplitude vectors were bit-exact [OFFLINE VERIFIED, DGX GB10 aarch64 unit].
   The receiver is NOT affected: `nr-uesoftmodem` exports `dft`/`idft` (`nm -D`), so it takes path 1, where the library
   was already initialised by `load_dftslib` [CODE-READ + nm, DGX build].
2. **ptxas (CUDA 13.0, sm_121) miscompiles the NEON twin's `flip()`.** `flip(a) = (a.y, xneg(a.x))` stores the negation
   as int16. NVVM turns the int-domain select of `nr_isa::xneg` into `cvt.u16 -> neg.s16 -> cvt.s32.s16`, and the PTX
   it emits is correct. ptxas then folds this into a 32-bit negation when the operand comes from a `[-32768, 32767]`
   clamp (`pcm2`), so -32768 becomes +32768. The bug sits in the dft64-level `(i)bfly4_16`: outputs y1/y3 are wrong
   whenever the twiddled sample saturates to -32768.
   Evidence [OFFLINE VERIFIED, DGX GB10 aarch64 unit + scratch diagnostics]:
   - Mismatched bins are exactly k ≡ 29 (mod 32) for one vector: dft16 bin 13, level-64 outputs j = 1 and 3.
   - Stand-alone dft64, GPU vs CPU `dft64`: 0 / 16 / 16899 / 30949 of 128000 outputs differ at amplitudes
     1000 / 4000 / 16000 / 32768.
   - A host scalar model of the level-64 stage, fed with the GPU's own dft16 output, matches the CPU 100 % and the GPU
     0 % on exactly those 16 outputs. In every failing case `x1t.re = -32768`.
   - A minimal 10-line kernel (clamp -> `xneg` -> stored as short -> subtract) reproduces it: 16385/65536 wrong.
     Its PTX is correct, so the fault is in ptxas.
   This is a GPU code-generation defect, not a numeric difference between GB10 and the CPU: once it is fixed the
   results are bit-identical.
3. Test harness, aarch64 only: the NEON libdfts references `write_file_matlab` and reads `g_log->dump_mask` in `dft128`.
   The test's stubs lacked the first and set `g_log = nullptr`, so dlopen failed with an undefined symbol (`cpu_dft=none`
   in a direct run) and then segfaulted in `dft128` once loaded.

[HYPOTHESIS] Before the merge, GB10 was "bit-exact, 0 mismatches" on recorded receiver data. The old kernel used the
same `(short)(-(int)x)` negation. The self-test added by `8d5a91f868` (written and validated on sens6, nvcc 12.4 sm_89)
added full-scale vectors that exercise the saturated case for the first time on GB10.

## Fix (no flags, no host constants, fail-closed unchanged, bit-exact check not loosened)

- `4a2c95d638` `nr_gpu_isa_ref.c`: open `libdfts.so` the way the loader does. A library that is already loaded is
  reused as is (`RTLD_NOLOAD`); a freshly opened one gets its `dfts_autoinit()` if it has one. The test now has a
  `write_file_matlab` stub and zeroed `g_log` storage.
- `e163ac69d7` `nr_pdsch_gpu_exact_fep.cuh`: `flip()` uses `neg16()`. It sign-extends the 32-bit negation with a `prmt`
  byte permute (sign-replicate selector `0x9910`), which neither compiler folds.

## x86 / sens6 selection after the fix

- [CODE-READ] `flip()` is only called by `r4_16`/`dft16`/`fft4096`, which together are the NEON DFT twin. The x86 twin
  (`fft_x86`, `xmj/xpj`, rotation twins) is not touched. The x86 libdfts has no `dfts_autoinit` symbol, so the reference
  change is a no-op there, and `RTLD_NOLOAD` only reuses a library the process already loaded.
- [OFFLINE VERIFIED, DGX build, cuobjdump] The SASS of every x86-profile kernel (`k_fep_exact<3>`, `k_delay_exact<3>`,
  `k_twin_dft<3,*>`, `k_twin_dft_x86n`, all `k_twin_rot`/`k_rot_n`) is byte-identical before and after the fix for
  both sm_89 and sm_121. Only the NEON-profile (P=1, P=2) kernels changed.
- Therefore on sens6 the x86 twin (`dft_twin=avx2 rot_twin=madd`) is still chosen, with identical results. Not run on
  x86 (no x86 host here); the claim rests on the code reading and the SASS comparison above.

## Test results [OFFLINE VERIFIED, DGX GB10 aarch64 unit, own build, serial, CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps]

- `test_nr_gpu_isa_twin`: `dft_twin=neon rot_twin=qrdmx`. dft4096 and idft4096 differ in 0/57344 outputs; the
  non-selected avx2 twin differs in 49072 and 49440. Rotation qrdmx: 0/174976 REs differ (v80 differs in 80307,
  madd in 67583). `k_fep_exact` 0/114688, `k_delay_exact` 0/12. PASS.
- Stand-alone dft64 after the fix: 0/128000 outputs differ at all four amplitudes.
- `ctest -R gpu -j1`: **44/44 passed**. The 2 skipped tests (`test_nr_pusch_gpu_ldpc_cpu`, `test_nr_pusch_gpu_fep_cpu`)
  return 77 by design. The tests that passed include `test_nr_gpu_pass`, `_discrete`, `_deq_direct`, `_deq_discrete`,
  `_oom`, `_compat` and `test_nr_pdsch_gpu_exact_size_512..4096`. The reference build showed 8 failures.
- Note: one earlier `ctest -R gpu` run had `test_nr_gpu_pass` failing 4 subtests (`WarmSetFitsBudget` reported
  `capped=5092`, plus 3 warm/UL subtests). Each passed in isolation, and the next 2 runs of `ctest -R '^test_nr_gpu_pass$'`
  and a full `ctest -R gpu` rerun passed. [HYPOTHESIS] The cause was transient memory pressure from other sessions on
  this shared unified-memory host. Watch for it: it is not attributable to this change.

## Open items

- The ptxas fold could hit any other int16 negation whose result is stored as short after a clamp. The other `xneg`
  call sites in the exact/UL FEP stay in int and pass. The x86 twin's `xmj/xpj` store as short but are validated on
  sm_89 and not selected on GB10. They were left unchanged on purpose, so the x86 SASS stays identical.
- No receiver/rfsim/OTA run (out of scope).
