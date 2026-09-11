#!/usr/bin/env bash
# adaptive_RX_pipeline.md, Stage 1 / P05 -- hardware-isolation guard.
#
# Branch code (nr_rx_branch.c, nr_rx_branch_sync.c, nr_rx_span_pool.c) is pure per-branch state:
# it must never call into the radio directly. There is no compiler-enforced way to say "this
# translation unit may not call these symbols" in C, so this script is the checked-in substitute:
# it greps the branch modules for forbidden hardware-access symbols and fails the build if any
# turn up. Register it in CMake with add_test so `ctest -R branch_hw_isolation` runs it on every
# build, the same way nr_rx_branch_test/nr_rx_span_pool_test are registered.
#
# Word-boundary matching (\b) is deliberate, not decoration: nr_rx_branch.c's real, legitimate
# nr_rx_branch_set_rf_discontinuity() contains the literal substring "rf_" (in "_rf_discontinuity")
# and would be a false positive under plain substring grep. \b requires a non-word character (or
# string start) immediately before the token, which "_rf_discontinuity" does not have (the
# preceding character is itself an underscore, a word character) -- so that legitimate function
# name is correctly left alone, while a genuine hazard like "UE->rf_map" (preceded by '>') still
# matches. Verified empirically against the real files before trusting this, not assumed.
set -u

FORBIDDEN_SYMBOLS='nrue_ru_read|trx_|openair0_|set_rx_freq|set_rx_gain|uhd|usrp|device_init|rf_'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." >/dev/null 2>&1 && pwd)"
BRANCH_MODULE_DIR="${REPO_ROOT}/openair1/PHY/NR_UE_TRANSPORT"
DEFAULT_FILES=(
  "${BRANCH_MODULE_DIR}/nr_rx_branch.c"
  "${BRANCH_MODULE_DIR}/nr_rx_branch_sync.c"
  "${BRANCH_MODULE_DIR}/nr_rx_span_pool.c"
)

# check_files <file...>: prints any forbidden-symbol hits (grep -n style) and returns the number
# of FILES that had at least one hit (0 = clean). Shared by the real check and the self-test so
# both exercise the identical pattern -- a self-test of a copy-pasted second regex would prove
# nothing about the one actually guarding the branch modules.
check_files() {
  local f hits=0
  for f in "$@"; do
    if [[ ! -f "$f" ]]; then
      echo "check_branch_hw_isolation: missing file: $f" >&2
      hits=$((hits + 1))
      continue
    fi
    local out
    out="$(grep -nE "\\b(${FORBIDDEN_SYMBOLS})" "$f")"
    if [[ -n "$out" ]]; then
      echo "check_branch_hw_isolation: FORBIDDEN hardware symbol in $f:"
      echo "$out" | sed 's/^/  /'
      hits=$((hits + 1))
    fi
  done
  return "$hits"
}

run_selftest() {
  local tmpfile
  tmpfile="$(mktemp /tmp/branch_hw_isolation_selftest.XXXXXX.c)"
  trap 'rm -f "$tmpfile"' RETURN
  cat > "$tmpfile" << 'PLANT'
/* planted violation: a branch module must never call this */
int bad(void) { return trx_read_func(NULL, 0, 0); }
PLANT
  # Also plant the false-positive trap in a SEPARATE clean file to prove the word-boundary fix
  # isn't accidentally over-broad in the other direction (i.e. it must NOT flag this).
  local cleanfile
  cleanfile="$(mktemp /tmp/branch_hw_isolation_selftest_clean.XXXXXX.c)"
  cat > "$cleanfile" << 'CLEAN'
void nr_rx_branch_set_rf_discontinuity(void) { /* legitimate name, not a hardware call */ }
CLEAN

  echo "check_branch_hw_isolation: self-test -- planted violation must be DETECTED"
  if check_files "$tmpfile"; then
    echo "check_branch_hw_isolation: SELF-TEST FAILED -- planted forbidden symbol was NOT detected" >&2
    rm -f "$cleanfile"
    return 1
  fi
  echo "check_branch_hw_isolation: self-test -- planted violation detected, as expected (PASS 1/2)"

  echo "check_branch_hw_isolation: self-test -- legitimate rf_-substring name must NOT be flagged"
  if ! check_files "$cleanfile"; then
    echo "check_branch_hw_isolation: SELF-TEST FAILED -- false positive on a legitimate symbol name" >&2
    rm -f "$cleanfile"
    return 1
  fi
  echo "check_branch_hw_isolation: self-test -- no false positive, as expected (PASS 2/2)"
  rm -f "$cleanfile"
  return 0
}

if [[ "${1:-}" == "--selftest" ]]; then
  run_selftest
  exit $?
fi

if check_files "${DEFAULT_FILES[@]}"; then
  echo "check_branch_hw_isolation: OK -- no forbidden hardware symbols in ${#DEFAULT_FILES[@]} branch module(s)"
  exit 0
else
  echo "check_branch_hw_isolation: FAILED -- see hits above" >&2
  exit 1
fi
