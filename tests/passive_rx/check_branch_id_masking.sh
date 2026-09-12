#!/usr/bin/env bash
# adaptive_RX_pipeline.md, P13b -- branch-id masking guard.
#
# P13a's fix rounds 2 and 3 found and fixed FOUR separate instances of ONE defect: a raw
# `branch_id & (NR_RX_BRANCH_MAX - 1)` mask used as an array index. The mask is silently wrong for
# the NR_ISAC_BRANCH_NONE sentinel -- 0xFF & 3 == 3 -- so a "this view maps to no active branch"
# value is credited to REAL branch 3. Every one of the four was found by a human reading the code,
# one at a time, one review round apart. The sanctioned form is nr_rx_branch_counter_index()
# (nr_rx_branch.h) / branch_engine_index() (pipeline_types.h), which return -1 for the sentinel
# instead of aliasing it; those helpers use range COMPARISONS, not the mask, so they do not
# self-flag here and need no exemption. This script is the checked-in substitute for a compiler
# that cannot express "do not index by this value": it greps the two module trees for the raw mask
# and fails the build if a new one appears. Registered in CMake with add_test the same way
# check_branch_hw_isolation.sh (P05) is registered.
#
# THE HEURISTIC, stated plainly because it is the whole basis of the check: a grep cannot know a
# variable's semantics, so "could this hold a branch id" is approximated by "the line mentions a
# `branch`-prefixed token" (word-boundary \bbranch, so `job.branch_id` / `->branch_id` match while
# `g_br_queued` or `nb_branches_seen` do not). The literal `& 3` form is included in the mask
# pattern -- NR_RX_BRANCH_MAX is 4 -- and is safe to include only BECAUSE of that heuristic: the
# two live `& 3` uses in these trees (nr_pbch.c `i_ssb & 3`, dci_nr.c `sc & 3`) name no branch
# token and are correctly ignored. Verified against the real files before trusting it, not assumed.
#
# Comment lines are skipped (`*`, `//`, `/*` leaders): three of the sites fixed in P13a carry a
# comment QUOTING the bad idiom to explain why it is banned, and flagging the documentation of a
# fix as the defect would make the guard unusable on its first day.
#
# A genuinely bounded use opts out EXPLICITLY, by carrying the marker `branch-mask-ok` on the line
# with a comment saying why it is bounded. Three exist today. Two are in
# nr_pdsch_passive_queue.c: they read the ENQUEUE-time branch_id, written only by a producer or by
# nr_rx_branch_set_dispatch() (active branches only) and always BEFORE the branch-view resolve
# that is the only writer able to put NR_ISAC_BRANCH_NONE in the field. The third is
# nr_passive_harq_tag.h's `% NR_RX_BRANCH_MAX`, whose sole call site maps the sentinel to lane 0
# before calling, so the reduction is a defensive backstop that the sentinel never reaches.
# Opt-out-with-a-reason is deliberate: it makes each exception a reviewable line rather than a
# silent hole in the pattern.
#
# KNOWN LIMIT, stated plainly rather than left implicit: this is a TEXTUAL check over source lines.
# It catches the mask written literally on one line, next to a branch-named token. It CANNOT see:
# the mask split across two lines, a mask hidden behind a macro (`#define BR_IDX(x) ((x) & 3)`),
# an equivalent expression that is neither covered form (`& 0x3` written in hex, say), or an
# unsafe index computed via a differently-named variable that was assigned from a branch id
# earlier. Conversely it CAN false-positive: the heuristic reads the whole line,
# so an unrelated mask whose trailing comment happens to contain the word `branch` would be flagged
# (hit while writing this script's own self-test fixture). That direction is cheap to resolve -- a
# reviewer reads one line and adds the marker or rewords the comment -- which is why the pattern is
# tuned to miss nothing rather than to never bother anyone. It is a guard against recurrence of the
# exact idiom that recurred four times, not a proof of absence.
set -u

# `& (NR_RX_BRANCH_MAX - 1)` with any spacing/parenthesisation, the literal `& 3`, or the
# equivalent `% NR_RX_BRANCH_MAX` reduction. Fix round 1 added the `%` form: the header used to
# list it as a known miss, and a LIVE instance exists (nr_passive_harq_tag.h, now exempted with
# its reason), so listing it as a limitation while it was reachable made the OK line untrustworthy.
MASK_PATTERN='(&[[:space:]]*\(?[[:space:]]*(NR_RX_BRANCH_MAX[[:space:]]*-[[:space:]]*1|3)[[:space:]]*\)?|%[[:space:]]*NR_RX_BRANCH_MAX)'
# The naming heuristic: the line must also mention a branch-prefixed token.
BRANCH_TOKEN='\bbranch'
# Explicit, reviewed opt-out marker.
EXEMPT_MARKER='branch-mask-ok'
# Comment-line leaders, anchored past grep -n's "<line>:" prefix (documentation of the
# banned idiom is not the idiom).
COMMENT_LEADER='^[0-9]+:[[:space:]]*(\*|//|/\*)'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." >/dev/null 2>&1 && pwd)"
SCAN_DIRS=(
  "${REPO_ROOT}/openair1/PHY/NR_UE_TRANSPORT"
  "${REPO_ROOT}/openair1/PHY/NR_UE_ISAC"
)

# check_files <file...>: prints any raw-mask hits (grep -n style) and returns the number of FILES
# that had at least one hit (0 = clean). Shared by the real check and the self-test so both
# exercise the identical pattern -- a self-test of a copy-pasted second regex would prove nothing
# about the one actually guarding the modules.
check_files() {
  local f hits=0
  for f in "$@"; do
    if [[ ! -f "$f" ]]; then
      echo "check_branch_id_masking: missing file: $f" >&2
      hits=$((hits + 1))
      continue
    fi
    local out
    out="$(grep -nE "${MASK_PATTERN}" "$f" \
           | grep -vE "${COMMENT_LEADER}" \
           | grep -v "${EXEMPT_MARKER}" \
           | grep -E "${BRANCH_TOKEN}")"
    if [[ -n "$out" ]]; then
      echo "check_branch_id_masking: RAW branch-id mask in $f -- use nr_rx_branch_counter_index() /"
      echo "  branch_engine_index(), or mark the line ${EXEMPT_MARKER} with the bounding argument:"
      echo "$out" | sed 's/^/  /'
      hits=$((hits + 1))
    fi
  done
  return "$hits"
}

collect_sources() {
  local d
  for d in "${SCAN_DIRS[@]}"; do
    [[ -d "$d" ]] || continue
    find "$d" -path '*/tests/*' -prune -o \( -name '*.c' -o -name '*.cc' -o -name '*.h' \) -print
  done | sort
}

run_selftest() {
  local tmp_dir
  tmp_dir="$(mktemp -d /tmp/branch_id_masking_selftest.XXXXXX)"
  trap 'rm -rf "$tmp_dir"' RETURN

  # (a) Planted raw masks on branch-named variables, outside the helpers, must be DETECTED.
  local -a plants=(
    'g_counter[job.branch_id & (NR_RX_BRANCH_MAX - 1)]++;|symbolic mask on job.branch_id'
    'g_counter[branch_id & 3]++;|literal & 3 mask on branch_id'
    'return tab[d->branch_id&(NR_RX_BRANCH_MAX-1)];|no-whitespace symbolic mask'
    'g_counter[branch_id % NR_RX_BRANCH_MAX]++;|modulo reduction (fix round 1)'
  )
  local i=0 entry line label f ok=0
  for entry in "${plants[@]}"; do
    i=$((i + 1))
    line="${entry%%|*}"
    label="${entry#*|}"
    f="${tmp_dir}/plant_${i}.c"
    printf '/* planted violation: the sentinel 0xFF would alias onto real branch 3 */\n%s\n' "$line" > "$f"
    echo "check_branch_id_masking: self-test -- planted violation ($label) must be DETECTED"
    if check_files "$f"; then
      echo "check_branch_id_masking: SELF-TEST FAILED -- $label was NOT detected" >&2
      return 1
    fi
    ok=$((ok + 1))
    echo "check_branch_id_masking: self-test -- $label detected, as expected"
  done

  # (b) The real helper headers are the SANCTIONED implementations and must not self-flag.
  local -a helpers=(
    "${REPO_ROOT}/openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch.h"
    "${REPO_ROOT}/openair1/PHY/NR_UE_ISAC/pipeline_types.h"
  )
  echo "check_branch_id_masking: self-test -- the helper headers themselves must NOT be flagged"
  if ! check_files "${helpers[@]}"; then
    echo "check_branch_id_masking: SELF-TEST FAILED -- the sanctioned helpers self-flagged" >&2
    return 1
  fi
  echo "check_branch_id_masking: self-test -- helpers clean, as expected"

  # (c) A mask on a variable that is not branch-id-shaped, a comment quoting the banned idiom, and
  # an explicitly exempted line must all be IGNORED -- this is the heuristic's false-positive side,
  # which is the half a purely syntactic pattern gets wrong.
  local cleanfile="${tmp_dir}/clean.c"
  cat > "$cleanfile" << 'CLEAN'
int nushift(int i_ssb) { return i_ssb & 3; }                 /* an SSB index, nothing to do here */
int sub(int sc) { return (sc & (4 - 1)) == 1; }              /* a subcarrier index, likewise */
/* documentation may quote the banned idiom: branch_id & (NR_RX_BRANCH_MAX - 1) is WRONG */
int bounded(void) { return g_q[job.branch_id & (NR_RX_BRANCH_MAX - 1)]; } /* branch-mask-ok: pre-resolve id */
int lane(int branch_id, int hpn) { return (branch_id % NR_RX_BRANCH_MAX) * 32 + hpn % 32; } /* branch-mask-ok: sentinel mapped at the call site */
int stride(int branch_hpn) { return branch_hpn % 32; }          /* a different modulus entirely */
CLEAN
  echo "check_branch_id_masking: self-test -- non-branch masks, a quoted idiom and an exempt line must NOT be flagged"
  if ! check_files "$cleanfile"; then
    echo "check_branch_id_masking: SELF-TEST FAILED -- false positive on a non-branch/exempt line" >&2
    return 1
  fi
  echo "check_branch_id_masking: self-test -- no false positive, as expected"

  echo "check_branch_id_masking: self-test -- ${ok}/${#plants[@]} planted violations detected, 0 false positives (PASS)"
  return 0
}

if [[ "${1:-}" == "--selftest" ]]; then
  run_selftest
  exit $?
fi

mapfile -t SOURCES < <(collect_sources)
if [[ ${#SOURCES[@]} -eq 0 ]]; then
  echo "check_branch_id_masking: FAILED -- no sources found under ${SCAN_DIRS[*]}" >&2
  exit 1
fi
if check_files "${SOURCES[@]}"; then
  echo "check_branch_id_masking: OK -- no raw branch-id masks in ${#SOURCES[@]} source file(s)"
  exit 0
else
  echo "check_branch_id_masking: FAILED -- see hits above" >&2
  exit 1
fi
