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
#
# Fix round 1 (2026-09-11, controller-ruled): the original forbidden-symbol list matched
# nrue_ru_read (the one radio READ the plan names verbatim) but NOT nrue_ru_set_freq() or
# nrue_ru_reinit() -- the two literal RETUNE/device-restart calls
# docs/passive_branch_wiring_plan.md's AcquisitionOwner table cites at nr-ue.c:2013-2014,2023 as
# exactly what "no branch-local correction may move the common hardware frequency" (P05's own
# rule) exists to prevent. Fixed by adding a `nrue_ru_` PREFIX pattern (the explicit
# `nrue_ru_read` entry is kept even though it is now a subset of the prefix, per the fix ruling's
# "keeping the existing entries") -- this covers every current and future nrue_ru_* radio
# entry point, not just the two named here, without needing another round of "found one more".
#
# KNOWN LIMIT, stated plainly rather than left implicit: this is a TEXTUAL check over the branch
# modules' own source lines. It catches a forbidden SYMBOL NAME appearing literally in that
# source. It CANNOT see: a forbidden call reached through a function pointer (e.g. a struct of
# callbacks populated elsewhere), a macro that expands to a forbidden symbol without the literal
# name appearing in these files, or a forbidden call added to a file this script isn't pointed at.
# This matches the brief's own specification (a checked-in script, not a build-system-level ban)
# and is recorded here as an accepted limit of that specification, not a surprise found later.
set -u

FORBIDDEN_SYMBOLS='nrue_ru_read|nrue_ru_|trx_|openair0_|set_rx_freq|set_rx_gain|uhd|usrp|device_init|rf_'

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
  local tmp_dir
  tmp_dir="$(mktemp -d /tmp/branch_hw_isolation_selftest.XXXXXX)"
  trap 'rm -rf "$tmp_dir"' RETURN

  # Each entry: "<planted line>|<label>". One file per entry, one DETECT assertion per entry --
  # fix round 1 adds the two retune calls the reviewer found missing (nrue_ru_set_freq,
  # nrue_ru_reinit) alongside the original trx_ example, so each newly-covered symbol has its own
  # proof rather than relying on the prefix match being exercised only indirectly.
  local -a plants=(
    'int bad(void) { return trx_read_func(NULL, 0, 0); }|trx_read_func (original)'
    'void bad(void) { nrue_ru_set_freq(NULL, 0, 0, 0); }|nrue_ru_set_freq (fix round 1)'
    'int bad(void) { return nrue_ru_reinit(); }|nrue_ru_reinit (fix round 1)'
  )
  local i=0 entry line label f ok=0
  for entry in "${plants[@]}"; do
    i=$((i + 1))
    line="${entry%%|*}"
    label="${entry#*|}"
    f="${tmp_dir}/plant_${i}.c"
    printf '/* planted violation: a branch module must never call this */\n%s\n' "$line" > "$f"
    echo "check_branch_hw_isolation: self-test -- planted violation ($label) must be DETECTED"
    if check_files "$f"; then
      echo "check_branch_hw_isolation: SELF-TEST FAILED -- $label was NOT detected" >&2
      return 1
    fi
    ok=$((ok + 1))
    echo "check_branch_hw_isolation: self-test -- $label detected, as expected"
  done

  # False-positive trap: a legitimate symbol name that happens to contain a forbidden substring
  # ("rf_" inside "_rf_discontinuity") must NOT be flagged.
  local cleanfile="${tmp_dir}/clean.c"
  cat > "$cleanfile" << 'CLEAN'
void nr_rx_branch_set_rf_discontinuity(void) { /* legitimate name, not a hardware call */ }
CLEAN
  echo "check_branch_hw_isolation: self-test -- legitimate rf_-substring name must NOT be flagged"
  if ! check_files "$cleanfile"; then
    echo "check_branch_hw_isolation: SELF-TEST FAILED -- false positive on a legitimate symbol name" >&2
    return 1
  fi
  echo "check_branch_hw_isolation: self-test -- no false positive, as expected"

  echo "check_branch_hw_isolation: self-test -- ${ok}/${#plants[@]} planted violations detected, 0 false positives (PASS)"
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
