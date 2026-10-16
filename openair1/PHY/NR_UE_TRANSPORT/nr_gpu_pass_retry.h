#pragma once
/* Task 8b, ISAC_GPU_PASS_RETRY_OLD=1 (default off): the retry / recovery bodies of a grant (every body after the first of the call, and every body of the
 * RECOVERY_ASYNC worker) skip the graph pass and run today's exact-GPU path. Why: a retry changes only the chest/demod configuration (S toggle, CSI-RS, CDM, wide MRC)
 * while the pass redoes FEP + chest + demod + LDPC behind one 256-512 us host wait; the old chain is ~355-405 us there (GPUPASS-WHY). The old path is the pass's
 * bit-exact reference, so the retry results are unchanged by construction. */
/* Assumption: body index > 1 means "retry". It is wrong when an earlier body was only a prediction or a diagnostic alternate (RETRY_PREDICT + TDRA_RETRY runs a quiet
 * predicted alt-S body first; QAM-AB / CSI-extent-AB alternates): `special` = t_quiet || t_qam_ab_alt || t_decode_tdra_attempt, such bodies keep the pass. The plain
 * body that follows a missed prediction is then still sent to the old path (correct, only slower). */
static inline int nr_gpu_pass_retry_to_old(int flag, unsigned bodies_in_call, int recov_worker, int special)
{
  return flag && !special && (bodies_in_call > 1 || recov_worker);
}
