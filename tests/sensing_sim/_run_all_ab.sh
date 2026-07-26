#!/bin/bash
# Runs all three pending A/B batches back to back (sequential -- netns names are fixed so they
# can't run in parallel): far_harmonic_reject, sync_correction, cfar_fa_target_max_det.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

REPEATS=${1:-3}
DUR=${2:-300}

./_run_mot_generic_ab.sh far_harmonic_reject 0 1 "$REPEATS" "$DUR" /tmp/mot_ab_far_harmonic_reject
./_run_mot_generic_ab.sh sync_correction 0 1 "$REPEATS" "$DUR" /tmp/mot_ab_sync_correction
./_run_mot_generic_ab.sh cfar_fa_target_max_det 8 60 "$REPEATS" "$DUR" /tmp/mot_ab_cfar_fa_target_max_det

echo "=== ALL A/B BATCHES DONE ==="
