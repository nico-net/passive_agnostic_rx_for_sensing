### Task 7: Result-preserving speed-up of the joint solver

Benchmark (`/home/sens/NICOLA/bench_joint`, memory `joint-solver-gpu-vs-cpu-benchmark`): ~50 µs per AL1 solve across distinct candidates but 6.4 µs when re-solving the same one, i.e. data-dependent branches in the elimination dominate. Order 0 also clears a 20 KB flip table it never reads and zero-inits a 1,728-byte array sized for AL16.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_joint_solve.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_joint_solve_test.cc`

**Interfaces:**
- Consumes: nothing new.
- Produces: no API change.

- [ ] **Step 1: Write the characterisation test**

Add to `tests/nr_pdcch_joint_solve_test.cc` before `int main`:

```cpp
TEST(JointSolve, OptimisationKeepsResultsBitIdentical)
{
  // Golden hash of every output field over a fixed corpus (AL1+AL2, signal and noise, orders 0-2),
  // recorded from the solver BEFORE any optimisation. A change to results, not just speed, fails it.
  std::mt19937 rng(20260925);
  uint64_t h = 1469598103934665603ULL;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
  for (int L : {1, 2}) {
    const int A = 51;
    EncCtx c{A, L};
    const int E = EncLen(A, L);
    auto *m = nr_pdcch_joint_model_new(RealEncode, &c, A, E, 64, 1);
    ASSERT_NE(m, nullptr);
    std::normal_distribution<double> nz(0.0, 0.354);
    for (int k = 0; k < 400; k++) {
      std::vector<int16_t> llr;
      if (k % 4 == 0) {
        const uint64_t pl = (((uint64_t)rng() << 32) | rng()) & Mask(A);
        const uint16_t rn = (uint16_t)(1 + rng() % 65519);
        llr = Channel(TxBits(pl, rn, 64, A, L, true), 6.0, rng);
      } else {
        llr.resize(E);
        for (auto &v : llr) v = (int16_t)std::max(-30000.0, std::min(30000.0, nz(rng) * 64.0));
      }
      for (int order = 0; order <= 2; order++) {
        nr_pdcch_joint_result_t r;
        nr_pdcch_joint_solve(m, llr.data(), order, &r);
        mix(r.payload); mix(r.rnti); mix(r.nid); mix((uint64_t)r.accepted); mix((uint64_t)r.mismatched_bits);
        mix((uint64_t)r.n_candidates); mix((uint64_t)std::llround(r.corr * 1e9));
        mix((uint64_t)std::llround(r.threshold * 1e9));
      }
    }
    nr_pdcch_joint_model_free(m);
  }
  printf("JOINT GOLDEN HASH = 0x%016llx\n", (unsigned long long)h);
  EXPECT_EQ(h, UINT64_C(0x0));  // Step 2 replaces 0x0 with the value printed by the UNMODIFIED solver
}
```

- [ ] **Step 2: Record the golden hash on the unmodified solver**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make test_nr_pdcch_joint_solve 2>&1 | tail -1 && ./test_nr_pdcch_joint_solve --gtest_filter='*BitIdentical*' 2>&1 | grep 'GOLDEN HASH'"`
Expected: one line `JOINT GOLDEN HASH = 0x…`. Replace `UINT64_C(0x0)` with that value, rebuild, rerun: the test must now PASS. Commit the test alone:

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git add openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_joint_solve_test.cc && git commit -F -" <<'EOF'
Joint solver: golden-hash characterisation test before optimising

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
EOF
```

- [ ] **Step 3: Apply the result-preserving optimisations**

In `nr_pdcch_joint_solve.c`, inside `nr_pdcch_joint_solve()`:

(a) Branchless MRIP elimination — replace

```c
    for (int p = 0; p < r; p++)
      if (v_get(v, piv[p]))
        v = v_xor(v, basis[p]);
```

(the occurrence inside `nr_pdcch_joint_solve`, not the one in `model_new_full`) with

```c
    for (int p = 0; p < r; p++) {
      const uint64_t msk = 0 - (uint64_t)v_get(v, piv[p]);
      v.w[0] ^= basis[p].w[0] & msk;
      v.w[1] ^= basis[p].w[1] & msk;
    }
```

(b) Branchless Gauss-Jordan elimination — replace

```c
    for (int q = 0; q < K; q++)
      if (q != c && v_get(Bm[q], c)) {
        Bm[q] = v_xor(Bm[q], Bm[c]);
        Iv[q] = v_xor(Iv[q], Iv[c]);
      }
```

with

```c
    const v128 bc = Bm[c], ic = Iv[c];
    for (int q = 0; q < K; q++) {
      const uint64_t msk = (0 - (uint64_t)v_get(Bm[q], c)) & (0 - (uint64_t)(q != c));
      Bm[q].w[0] ^= bc.w[0] & msk; Bm[q].w[1] ^= bc.w[1] & msk;
      Iv[q].w[0] ^= ic.w[0] & msk; Iv[q].w[1] ^= ic.w[1] & msk;
    }
```

(c) Clear the flip tables only when they are used — replace

```c
  ebits d0 = {{0}}, T[JMAX_K];
  memset(T, 0, sizeof(T));
```

with

```c
  ebits d0 = {{0}}, T[JMAX_K];
  if (order >= 1)
    memset(T, 0, sizeof(T)); /* only orders 1/2 read T */
```

(d) Replace the AL16-sized byte array with a bitmask — replace

```c
  uint8_t inI[NR_PDCCH_JOINT_MAX_E] = {0};
  for (int t = 0; t < K; t++)
    inI[I[t]] = 1;
```

with

```c
  ebits inIb = {{0}};
  for (int t = 0; t < K; t++)
    e_flip(&inIb, I[t]); /* I[] holds distinct positions */
```

and change every remaining `inI[i]` in the function (the `WO/W2O` loop and the `EVAL` macro) to `e_get(&inIb, i)`. Do NOT change the `qsort` call — tie order among equal reliabilities affects which basis is chosen, hence results.

- [ ] **Step 4: Verify results are unchanged**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j12 test_nr_pdcch_joint_solve test_nr_pdcch_joint_live 2>&1 | tail -1 && ctest -R 'test_nr_pdcch_joint' --output-on-failure | tail -4"`
Expected: both pass, including `OptimisationKeepsResultsBitIdentical`. If the golden hash fails, revert the step that changed it (bisect (a)–(d)) — do not update the hash.

- [ ] **Step 5: Measure**

Run: `ssh sens6 "cd /home/sens/NICOLA/bench_joint && bash build.sh >/dev/null && ./bench 51 2>&1 | grep -v '^\[' | sed -n '1,12p'"`
Record the table next to the pre-change one (memory `joint-solver-gpu-vs-cpu-benchmark`: 1-thread 54,339 µs and 6-thread 10,501 µs at 1,080 candidates). Keep the change only if 1-thread time at 1,080 candidates drops by ≥ 20 %; otherwise `git checkout -- openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_joint_solve.c` and record the null result in the memory file.

- [ ] **Step 6: Commit (only if kept)**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_joint_solve.c && git commit -F -" <<'EOF'
Joint solver: branchless GF(2) elimination; skip order-0 work it never reads

Measured ~50 us per AL1 solve across distinct candidates vs 6.4 us re-solving one: mispredicts in
the data-dependent elimination dominated. Masks replace the branches; order 0 no longer clears the
20 KB flip table or an AL16-sized byte array. Results bit-identical (golden-hash test).
<paste the before/after bench rows here>

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
EOF
```

---

