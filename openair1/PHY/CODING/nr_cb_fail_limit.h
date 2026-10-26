/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* The one place of the transport-block abort rule (operator's choice, 2026-10-26): the decode of a TB's code blocks
 * stops once this many code blocks have FAILED (the blocks not yet started then count as failed, not tried); it
 * continues past fewer failures. It was 1 (the first failed block aborted the rest). The TB still passes only if every
 * block and the TB CRC pass. Plain C, no includes: read by the CPU decoder (coding_defs.h) and by the CUDA graph pass. */
#ifndef NR_CB_FAIL_LIMIT_H
#define NR_CB_FAIL_LIMIT_H
#define NR_CB_FAIL_LIMIT 5
#endif
