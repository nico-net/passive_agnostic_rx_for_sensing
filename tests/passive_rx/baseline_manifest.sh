#!/usr/bin/env bash
# P01: read-only baseline manifest generator. Never builds, never runs the radio, never invokes
# tests/passive_rx/run_adaptive_receive_test.sh (that launcher opens the radio). Mirrors -- does
# not modify -- that launcher's identity-hash logic (its lines ~15-60) so the two stay comparable.
#
# Writes tests/passive_rx/baselines/manifest_<YYYYMMDD>_<commit7>[-dirty].json plus a sibling
# "<same-base>_tracked.patch" (the `git diff --binary` this manifest's hash refers to).
#
# Env: PURPOSE="..." sets experiment_purpose (default below).
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="$REPO/cmake_targets/ran_build/build"
SCRIPT_DIR="$REPO/tests/passive_rx"
cd "$REPO"

# ---- small JSON helpers (pure bash/sed; final document is validated+pretty-printed by
# `python3 -m json.tool` at the bottom, which also catches any escaping mistake here). ----
jesc() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | sed -e ':a;N;$!ba;s/\n/\\n/g'; }
jstr() { if [ -z "${1+x}" ] || [ "$1" = "__NULL__" ]; then printf 'null'; else printf '"%s"' "$(jesc "$1")"; fi; }
jnum_or_null() { if [ -z "$1" ]; then printf 'null'; else printf '%s' "$1"; fi; }

# ---- citations: every file:line reference in this manifest is DERIVED by grep at generation
# time, never hand-typed, per the P01 review's Important finding (hand-typed ranges drifted: e.g.
# a prior "2485-2494" for a call that actually runs to 2496). Each citation is stored as a
# structured {file, line_start, line_end, token} object; check_manifest.py re-verifies that
# `token` still occurs within [line_start, line_end] of the live file.
#
# citation FILE START_TOKEN [END_TOKEN] [MAX_SPAN=20]
#   Prints a TSV: file<TAB>line_start<TAB>line_end<TAB>token (token = START_TOKEN, exactly as
#   grepped for -- this IS the string check_manifest.py re-searches for). Single-line citation if
#   END_TOKEN is omitted. Fails loudly (CITATION_ERROR to stderr, causing `set -e`-visible garbage
#   line numbers of 0) if START_TOKEN isn't found -- never silently emits a stale/guessed range.
citation() {
  local file="$1" start_token="$2" end_token="${3:-}" max_span="${4:-20}"
  local abs="$REPO/$file"
  local start_line
  start_line=$(grep -nF -- "$start_token" "$abs" | head -1 | cut -d: -f1)
  if [ -z "$start_line" ]; then
    echo "CITATION_ERROR: token not found in $file: $start_token" >&2
    printf '%s\t%s\t%s\t%s' "$file" "0" "0" "$start_token"
    return
  fi
  local end_line="$start_line"
  if [ -n "$end_token" ]; then
    local rel
    rel=$(sed -n "$((start_line + 1)),$((start_line + max_span))p" "$abs" \
          | grep -nF -- "$end_token" | head -1 | cut -d: -f1)
    if [ -n "$rel" ]; then
      end_line=$((start_line + rel))
    else
      # Loud, not silent: falling back to a single-line range here would still pass
      # check_manifest.py's re-verification (it only re-checks start_token, which IS at
      # line_start) while quietly recording a too-narrow range. Surface it instead.
      echo "CITATION_ERROR: end_token not found within $max_span lines of $file:$start_line: $end_token" >&2
    fi
  fi
  printf '%s\t%s\t%s\t%s' "$file" "$start_line" "$end_line" "$start_token"
}

# Turns a `citation` TSV result into a compact JSON object.
citation_json() {
  local tsv="$1" cfile cstart cend ctoken
  IFS=$'\t' read -r cfile cstart cend ctoken <<< "$tsv"
  printf '{"file":%s,"line_start":%s,"line_end":%s,"token":%s}' \
    "$(jstr "$cfile")" "$cstart" "$cend" "$(jstr "$ctoken")"
}

BRANCH=$(git branch --show-current)
COMMIT=$(git rev-parse HEAD)
COMMIT7=${COMMIT:0:7}
STATUS_SHORT=$(git status --short || true)
DIRTY_SUFFIX=""
[ -n "$STATUS_SHORT" ] && DIRTY_SUFFIX="-dirty"
DATE_TAG=$(date -u +%Y%m%d)
GENERATED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
PURPOSE="${PURPOSE:-P01 baseline identity; no capture}"

OUTDIR="$SCRIPT_DIR/baselines"
mkdir -p "$OUTDIR"
BASE_NAME="manifest_${DATE_TAG}_${COMMIT7}${DIRTY_SUFFIX}"
MANIFEST="$OUTDIR/${BASE_NAME}.json"
PATCH="$OUTDIR/${BASE_NAME}_tracked.patch"

git diff --binary > "$PATCH"
PATCH_SHA=$(sha256sum "$PATCH" | awk '{print $1}')
PATCH_LINES=$(wc -l < "$PATCH" | tr -d ' ')

# ---- git status lines as a JSON array ----
STATUS_ARR="["
first=1
while IFS= read -r line; do
  [ -z "$line" ] && continue
  [ $first -eq 0 ] && STATUS_ARR+=","
  STATUS_ARR+="$(jstr "$line")"
  first=0
done <<< "$STATUS_SHORT"
STATUS_ARR+="]"

# ---- scoped source-file hashes: the EXACT ls-files invocation
# tests/passive_rx/run_adaptive_receive_test.sh (~line 51-54) uses to build source_files.sha256.
# Mirrored read-only; that launcher itself is never invoked. ----
FILES_ARR="["
first=1
while IFS= read -r -d '' f; do
  [ -f "$f" ] || continue
  sha=$(sha256sum -- "$f" | awk '{print $1}')
  [ $first -eq 0 ] && FILES_ARR+=","
  FILES_ARR+="{\"path\":$(jstr "$f"),\"sha256\":$(jstr "$sha")}"
  first=0
done < <(git ls-files -z --cached --others --exclude-standard -- openair1 openair2 executables radio tests/passive_rx)
FILES_ARR+="]"

# ---- build/driver identity ----
BIN="$BUILD/nr-uesoftmodem"
DRV="$BUILD/liboai_usrpdevif.so"
BIN_SHA=$(sha256sum "$BIN" | awk '{print $1}')
DRV_SHA=$(sha256sum "$DRV" | awk '{print $1}')
BIN_SIZE=$(stat -c '%s' "$BIN"); BIN_MTIME_EPOCH=$(stat -c '%Y' "$BIN")
DRV_SIZE=$(stat -c '%s' "$DRV"); DRV_MTIME_EPOCH=$(stat -c '%Y' "$DRV")
BIN_MTIME_UTC=$(date -u -d "@$BIN_MTIME_EPOCH" +%Y-%m-%dT%H:%M:%SZ)
DRV_MTIME_UTC=$(date -u -d "@$DRV_MTIME_EPOCH" +%Y-%m-%dT%H:%M:%SZ)
DEVICE_SYMLINK_TARGET=$(readlink "$BUILD/liboai_device.so" 2>/dev/null || echo "__NULL__")

# Same staleness rule the launcher enforces (run_adaptive_receive_test.sh ~lines 13-18): a tracked
# source under openair1/openair2/executables/radio (excluding */tests/*) newer than the binary.
# Recorded, not enforced -- this generator never blocks.
STALE_FILE=$(find "$REPO/openair1" "$REPO/openair2" "$REPO/executables" "$REPO/radio" \
  -path '*/tests/*' -prune -o \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \) \
  -newer "$BIN" -print -quit 2>/dev/null || true)
if [ -n "$STALE_FILE" ]; then STALE_VERDICT="stale"; else STALE_VERDICT="not_stale"; fi

# ---- CMake cache ----
CACHE="$BUILD/CMakeCache.txt"
cache_get() { grep -m1 "^$1:" "$CACHE" | cut -d= -f2- || true; }
CMAKE_BUILD_TYPE=$(cache_get CMAKE_BUILD_TYPE)
CMAKE_GENERATOR=$(cache_get CMAKE_GENERATOR)
ENABLE_ISAC_SENSING=$(cache_get ENABLE_ISAC_SENSING)
ENABLE_TESTS=$(cache_get ENABLE_TESTS)
ENABLE_CHANNEL_SIM_CUDA=$(cache_get ENABLE_CHANNEL_SIM_CUDA)
CMAKE_C_COMPILER=$(cache_get CMAKE_C_COMPILER)
CMAKE_CXX_COMPILER=$(cache_get CMAKE_CXX_COMPILER)
CC_VERSION=$("$CMAKE_C_COMPILER" --version 2>&1 | head -1)
CXX_VERSION=$("$CMAKE_CXX_COMPILER" --version 2>&1 | head -1)

if command -v uhd_config_info >/dev/null 2>&1; then
  UHD_VERSION=$(uhd_config_info --version 2>&1 | head -1)
  UHD_VERSION_SOURCE="uhd_config_info --version"
else
  UHD_VERSION=$(ldd "$DRV" | grep -oP 'libuhd\.so\S*' | head -1)
  UHD_VERSION_SOURCE="ldd liboai_usrpdevif.so"
fi

# ---- branch guard (run_adaptive_receive_test.sh ~lines 8-11) ----
case "$BRANCH" in
  adaptive-rx-UL-DL|merge/adaptive-sensing) BRANCH_GUARD_VERDICT="PASS: branch '$BRANCH' is in the launcher's accepted set" ;;
  *) BRANCH_GUARD_VERDICT="BLOCKED: branch '$BRANCH' is not in the launcher's accepted set {adaptive-rx-UL-DL, merge/adaptive-sensing}" ;;
esac

# ---- per-configuration-case parsing ----
# Replicates (does not run) the launcher's own guard grep (~lines 22-27):
#   enable=1 conf requires ENABLE_ISAC_SENSING:BOOL=ON; enable=0 conf requires ...=OFF.
guard_verdict() {
  local enable_v="$1"
  if [ "$enable_v" = "1" ]; then
    if [ "$ENABLE_ISAC_SENSING" = "ON" ]; then
      echo "PASS: enable=1 requires ENABLE_ISAC_SENSING:BOOL=ON; cache has ON"
    else
      echo "BLOCKED: enable=1 requires ENABLE_ISAC_SENSING:BOOL=ON; cache has $ENABLE_ISAC_SENSING"
    fi
  else
    if [ "$ENABLE_ISAC_SENSING" = "OFF" ]; then
      echo "PASS: enable=0 requires ENABLE_ISAC_SENSING:BOOL=OFF; cache has OFF"
    else
      echo "BLOCKED: enable=0 requires ENABLE_ISAC_SENSING:BOOL=OFF; cache has $ENABLE_ISAC_SENSING"
    fi
  fi
}

# Emits one compact JSON object for tests/passive_rx/<name>, or {"conf":null,"reason":...} if
# the file does not exist. Runs in a command-substitution subshell, so it cannot set variables
# visible to the caller -- conf_full_auto() below is the (non-subshell) way to get full_auto out.
conf_json() {
  local name="$1" relpath="tests/passive_rx/$1" f
  f="$REPO/$relpath"
  if [ ! -f "$f" ]; then
    printf '{"conf":null,"reason":"no config exists"}'
    return
  fi
  local sha; sha=$(sha256sum "$f" | awk '{print $1}')
  local enable_v; enable_v=$(grep -oP '^\s*enable\s*=\s*\K[0-9]+' "$f" | head -1 || true)
  local full_auto_v full_auto_note
  full_auto_v=$(grep -oP 'pdcch_blind_monitor_full_auto\s*=\s*\K[0-9]+' "$f" | head -1 || true)
  if [ -z "$full_auto_v" ]; then
    full_auto_note='"absent from conf; parser default is 0 (openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1355-1364, g_cfg.dl_full_auto .defintval=0) -- effective value is therefore 0"'
    full_auto_v=0
  else
    full_auto_note="null"
  fi
  local admissible="false"; [ "$full_auto_v" = "0" ] && admissible="true"

  local dci01_raw ul_pusch_raw pdsch_raw sources_raw
  dci01_raw=$(grep -oP 'pdcch_blind_monitor_dci01\s*=\s*"\K[^"]*' "$f" | head -1 || true)
  ul_pusch_raw=$(grep -oP 'pdcch_blind_monitor_ul_pusch\s*=\s*"\K[^"]*' "$f" | head -1 || true)
  pdsch_raw=$(grep -oP 'pdcch_blind_monitor_pdsch\s*=\s*"\K[^"]*' "$f" | head -1 || true)
  sources_raw=$(grep -oP '^\s*sources\s*=\s*"\K[^"]*' "$f" | head -1 || true)
  local dci01_scan="${dci01_raw%%:*}" ul_pusch_decode="${ul_pusch_raw%%:*}" pdsch_decode="${pdsch_raw%%:*}"

  printf '{"conf":%s,"sha256":%s,"measured":{"sensing_enable":%s,"pdcch_blind_monitor_full_auto":%s,"pdcch_blind_monitor_full_auto_note":%s,"admissible_test_mode":%s,"dl_decode":{"pdcch_blind_monitor_pdsch_raw":%s,"decode_field":%s},"ul_decode":{"pdcch_blind_monitor_dci01_raw":%s,"dci01_scan_field":%s,"pdcch_blind_monitor_ul_pusch_raw":%s,"ul_pusch_decode_field":%s},"sensing_sources":%s,"launcher_sensing_build_guard_verdict":%s,"launcher_branch_guard_verdict":%s}}' \
    "$(jstr "$relpath")" "$(jstr "$sha")" "$(jnum_or_null "$enable_v")" \
    "$full_auto_v" "$full_auto_note" "$admissible" \
    "$(jstr "$pdsch_raw")" "$(jnum_or_null "$pdsch_decode")" \
    "$(jstr "$dci01_raw")" "$(jnum_or_null "$dci01_scan")" "$(jstr "$ul_pusch_raw")" "$(jnum_or_null "$ul_pusch_decode")" \
    "$(jstr "$sources_raw")" "$(jstr "$(guard_verdict "${enable_v:-0}")")" "$(jstr "$BRANCH_GUARD_VERDICT")"
}

# Append an extra (already-formatted, leading-comma) fields string just before a compact JSON
# object's closing brace.
inject() { local json="$1" extra="$2"; printf '%s%s}' "${json%\}}" "$extra"; }

# Effective full_auto for tests/passive_rx/<name> (0 if absent, per the parser default cited above).
conf_full_auto() {
  local v
  v=$(grep -oP 'pdcch_blind_monitor_full_auto\s*=\s*\K[0-9]+' "$REPO/tests/passive_rx/$1" | head -1 || true)
  printf '%s' "${v:-0}"
}

NO_HINTS_JSON=$(conf_json adaptive_no_hints.conf)
MANUAL_DIAG_JSON=$(conf_json adaptive_manual_diag.conf)
MANUAL_DIAG_FULL_AUTO=$(conf_full_auto adaptive_manual_diag.conf)
PINNED_UL_JSON=$(conf_json adaptive_pinned_ul.conf)
AOA_TRACK_JSON=$(conf_json aoa_track_dl.conf)
MANUAL_DLUL_JSON=$(conf_json adaptive_manual_dlul.conf)

# ---- verify the plan's claim that aoa_track_dl.conf's rx_array fails the rank-two parser.
# Parser under test: openair1/PHY/NR_UE_ISAC/nr_isac.cc:102-125 (function parse_array), the
# rank-two condition itself at :118 (`if (!(eig.values[1] > tol) || eig.values[0] > tol) return
# false;` -- requires the 3x3 Gram matrix of baseline vectors to have exactly two significant
# eigenvalues). This independently checks the weaker, sufficient condition "the baseline vectors
# are exactly collinear" (rank 1) via plain Gaussian elimination (stdlib only, no numpy) rather
# than reproducing the source's own eigen-decomposition tolerance. Collinear implies rank<=1<2,
# which is enough to make the cited condition fail regardless of tol. ----
RX_ARRAY_RAW=$(grep -oP '^\s*rx_array\s*=\s*"\K[^"]*' "$REPO/tests/passive_rx/aoa_track_dl.conf" | head -1 || true)
RANK_CHECK_OUT=$(python3 - "$RX_ARRAY_RAW" <<'PYEOF'
import sys
spec = sys.argv[1]
pts = []
for tok in spec.split(';'):
    parts = [float(x) for x in tok.split(',')]
    while len(parts) < 3:
        parts.append(0.0)
    pts.append(parts[:3])
if len(pts) != 4:
    print("ERROR: expected 4 elements, got %d" % len(pts)); sys.exit(1)
baselines = [[pts[i][k] - pts[0][k] for k in range(3)] for i in (1, 2, 3)]

def rank(mat, tol=1e-9):
    m = [row[:] for row in mat]
    rows, cols = len(m), len(m[0])
    r = 0
    for col in range(cols):
        piv = None
        for i in range(r, rows):
            if abs(m[i][col]) > tol:
                piv = i; break
        if piv is None:
            continue
        m[r], m[piv] = m[piv], m[r]
        pv = m[r][col]
        m[r] = [v / pv for v in m[r]]
        for i in range(rows):
            if i != r and abs(m[i][col]) > tol:
                f = m[i][col]
                m[i] = [a - f * b for a, b in zip(m[i], m[r])]
        r += 1
    return r

rk = rank(baselines)
print("points=%s" % pts)
print("baseline_vectors=%s" % baselines)
print("gaussian_elimination_rank=%d" % rk)
print("rank_two_array_parser_condition_would=%s" % ("FAIL (rank<2, matches plan's claim)" if rk < 2 else "PASS (rank==2 or higher, contradicts plan's claim)" if rk >= 2 else "?"))
PYEOF
)
RANK_VERDICT="unverified"
if printf '%s' "$RANK_CHECK_OUT" | grep -q 'FAIL (rank<2'; then RANK_VERDICT="verified_fails_rank_two"; fi
if printf '%s' "$RANK_CHECK_OUT" | grep -q 'PASS (rank==2'; then RANK_VERDICT="verified_passes_rank_two"; fi

PARSER_CITATION=$(citation_json "$(citation openair1/PHY/NR_UE_ISAC/nr_isac.cc \
  'bool parse_array(const char* spec, double rotation_deg, const char* broadside_spec,' \
  'out->broadside=broadside;out->configured=true;return true;' 30)")
CONDITION_CITATION=$(citation_json "$(citation openair1/PHY/NR_UE_ISAC/nr_isac.cc \
  'if (!(eig.values[1] > tol) || eig.values[0] > tol) return false;')")

RANK_EXTRA=$(printf ',"rank_two_array_check":{"rx_array_raw":%s,"parser_function":"parse_array","parser_citation":%s,"parser_citation_note":"line_end is the last body statement, one line before the closing brace -- a bare closing brace alone is not a uniquely locatable token","rank_two_condition_citation":%s,"independent_check_method":"Gaussian elimination on the 3 baseline vectors p1-p0,p2-p0,p3-p0 (stdlib only); collinearity (rank<2) is a sufficient condition for the cited eigenvalue test to fail regardless of its numerical tolerance","verdict":%s,"raw_output":%s}' \
  "$(jstr "$RX_ARRAY_RAW")" "$PARSER_CITATION" "$CONDITION_CITATION" "$(jstr "$RANK_VERDICT")" "$(jstr "$RANK_CHECK_OUT")")
AOA_TRACK_JSON=$(inject "$AOA_TRACK_JSON" "$RANK_EXTRA")

ACQ_ONLY_JSON='{"conf":null,"reason":"no config exists"}'

# ---- antenna mapping / geometry (source-derived facts only; the physical survey does not exist).
# Every citation below is derived by `citation()` (grep at generation time), not hand-typed. ----
USRP_ARGS_CITATION=$(citation_json "$(citation tests/passive_rx/run_adaptive_receive_test.sh \
  '--usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174')")
CHANLIST_CITATION=$(citation_json "$(citation tests/passive_rx/run_adaptive_receive_test.sh \
  '--ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx')")
RFCHAN_CITATION=$(citation_json "$(citation radio/USRP/usrp_lib.cpp \
  'RFCHAN ch%d subdev=%s port=%s gain=%.2f range=[%.1f..%.1f] freq=%.6f MHz ' \
  'get_rx_bandwidth(i + choffset) / 1e6);')")

# ---- timestamp units: every citation derived by grep at generation time (see `citation()`) ----
SUBMIT_DEF_CITATION=$(citation_json "$(citation openair1/PHY/NR_UE_ISAC/sensing_engine.cc \
  'void SensingEngine::submit(uint32_t slot, float fraction, nr_isac_source_t source,' \
  'uint32_t re, float noise)')")
UTC_NS_ASSIGN_CITATION=$(citation_json "$(citation openair1/PHY/NR_UE_ISAC/sensing_engine.cc \
  'value->utc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(' \
  'std::chrono::system_clock::now().time_since_epoch()).count();')")
UTC_NS_FIELD_CITATION=$(citation_json "$(citation openair1/PHY/NR_UE_ISAC/sensing_engine.cc \
  'int64_t utc_ns = 0;')")
REPORT_JSON_UTC_CITATION=$(citation_json "$(citation openair1/PHY/NR_UE_ISAC/report_writer.cc \
  'cpi_start_time_utc_ns')")
ROW_TIME_CITATION=$(citation_json "$(citation openair1/PHY/NR_UE_ISAC/sensing_engine.cc \
  'report.first_row_time_ns = std::llround(dl_window.row_time_slots.front() * slot_ns);' \
  'report.cpi_duration_ns = std::max<int64_t>(0, report.last_row_time_ns - report.first_row_time_ns);')")
MIDPOINT_CITATION=$(citation_json "$(citation openair1/PHY/NR_UE_ISAC/sensing_engine.cc \
  'const double midpoint_slots = 0.5 * (dl_window.row_time_slots.front()' \
  '* slot_duration_s(dl_window.scs_hz);')")

cat > "$MANIFEST.tmp" <<JSONDOC
{
"schema": "p01.baseline_manifest.v1",
"generated_at_utc": $(jstr "$GENERATED_AT"),
"experiment_purpose": $(jstr "$PURPOSE"),
"host": "sens6",
"tree": $(jstr "$REPO"),
"git": {
  "branch": $(jstr "$BRANCH"),
  "commit": $(jstr "$COMMIT"),
  "status_short": $STATUS_ARR,
  "tracked_diff_patch_path": $(jstr "tests/passive_rx/baselines/${BASE_NAME}_tracked.patch"),
  "tracked_diff_patch_sha256": $(jstr "$PATCH_SHA"),
  "tracked_diff_patch_line_count": $PATCH_LINES,
  "scoped_files": $FILES_ARR
},
"build": {
  "nr_uesoftmodem": {"path":"cmake_targets/ran_build/build/nr-uesoftmodem","sha256":$(jstr "$BIN_SHA"),"size_bytes":$BIN_SIZE,"mtime_utc":$(jstr "$BIN_MTIME_UTC"),"mtime_epoch":$BIN_MTIME_EPOCH},
  "liboai_usrpdevif": {"path":"cmake_targets/ran_build/build/liboai_usrpdevif.so","sha256":$(jstr "$DRV_SHA"),"size_bytes":$DRV_SIZE,"mtime_utc":$(jstr "$DRV_MTIME_UTC"),"mtime_epoch":$DRV_MTIME_EPOCH},
  "liboai_device_so_symlink_target": $(jstr "$DEVICE_SYMLINK_TARGET"),
  "binary_stale_vs_tracked_source": {"verdict": $(jstr "$STALE_VERDICT"), "method": "find openair1 openair2 executables radio -path */tests/* -prune -o -name *.{c,h,cpp,cc} -newer nr-uesoftmodem (same rule as run_adaptive_receive_test.sh lines ~13-18; recorded, not enforced by this generator)", "first_newer_file": $(jstr "${STALE_FILE:-__NULL__}")}
},
"cmake_cache": {
  "CMAKE_BUILD_TYPE": $(jstr "$CMAKE_BUILD_TYPE"),
  "CMAKE_GENERATOR": $(jstr "$CMAKE_GENERATOR"),
  "ENABLE_ISAC_SENSING": $(jstr "$ENABLE_ISAC_SENSING"),
  "ENABLE_TESTS": $(jstr "$ENABLE_TESTS"),
  "ENABLE_CHANNEL_SIM_CUDA": $(jstr "$ENABLE_CHANNEL_SIM_CUDA"),
  "CMAKE_C_COMPILER": $(jstr "$CMAKE_C_COMPILER"),
  "CMAKE_C_COMPILER_VERSION": $(jstr "$CC_VERSION"),
  "CMAKE_CXX_COMPILER": $(jstr "$CMAKE_CXX_COMPILER"),
  "CMAKE_CXX_COMPILER_VERSION": $(jstr "$CXX_VERSION"),
  "uhd_version": $(jstr "$UHD_VERSION"),
  "uhd_version_source": $(jstr "$UHD_VERSION_SOURCE")
},
"configuration_cases": {
  "decoding-only": {
    "primary_conf": "tests/passive_rx/adaptive_manual_diag.conf",
    "primary_conf_rationale": "plan amendment 2026-09-11 (TESTING MODE RULE): manual mode only, full_auto=0; adaptive_manual_diag.conf is the admissible decoding-only conf",
    "candidates": {
      "adaptive_manual_diag.conf": $MANUAL_DIAG_JSON,
      "adaptive_pinned_ul.conf": $PINNED_UL_JSON,
      "adaptive_no_hints.conf": $NO_HINTS_JSON
    },
    "note": "adaptive_no_hints.conf is decoding-shaped (sensing.enable=0, DL+UL blind decode on) but full_auto=1 (VOID per the amendment) and is the launcher's own default CONF; adaptive_pinned_ul.conf is an admissible (full_auto=0) variant with a pinned UL DCI layout, not the case-defining conf"
  },
  "sensing-enabled": {
    "primary_conf": "tests/passive_rx/aoa_track_dl.conf",
    "candidates": {
      "aoa_track_dl.conf": $AOA_TRACK_JSON,
      "adaptive_manual_dlul.conf": $MANUAL_DLUL_JSON
    },
    "note": "aoa_track_dl.conf enables sensing (enable=1) but disables all UL blind decode (dci01=0, ul_pusch=0) and its rx_array fails the rank-two AoA parser (see candidates.aoa_track_dl.conf.rank_two_array_check); adaptive_manual_dlul.conf enables sensing with both DL and UL blind decode on and a pinned dedicated CORESET/SS/BWP. Both have pdcch_blind_monitor_pdsch decode_field=1 ('decode+count CRC pass rate only'), NOT 2 ('also submit CFR'), while their own sources= list includes pdsch_data -- recorded as measured, not resolved further; P01 is identity/config recording, not decode-path validation"
  },
  "acquisition-only": $ACQ_ONLY_JSON
},
"selected_test_conf": {
  "conf": "tests/passive_rx/adaptive_manual_diag.conf",
  "case": "decoding-only",
  "pdcch_blind_monitor_full_auto": $MANUAL_DIAG_FULL_AUTO,
  "rationale": "plan amendment 2026-09-11: manual mode (full_auto=0) is the only admissible test mode; check_manifest.py rejects a manifest whose selected_test_conf.pdcch_blind_monitor_full_auto != 0"
},
"antenna_mapping_and_geometry": {
  "status": "UNSURVEYED",
  "reason": "No separated-antenna physical survey has been performed or supplied as of $DATE_TAG (adaptive_RX_pipeline_progress.md 'Current status' table: 'Separated-antenna survey: Not provided or verified'). Do not infer physical channel-to-position mapping from source or config alone.",
  "known_from_source": {
    "usrp_args": "type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174",
    "usrp_args_citation": $USRP_ARGS_CITATION,
    "channel_list_flags": "--ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx",
    "channel_list_citation": $CHANLIST_CITATION,
    "rfchan_diagnostic": {
      "citation": $RFCHAN_CITATION,
      "log_tag": "RFCHAN",
      "fields_read_back_from_device": "subdev, rx_antenna (port), gain, gain_range, freq, rate, bandwidth -- read back FROM THE DEVICE post-configuration, not echoed from the config struct"
    }
  }
},
"timestamp_units": {
  "cpi_start_time_utc_ns": {
    "unit": "nanoseconds since Unix epoch",
    "clock_source": "host wall clock, std::chrono::system_clock::now(), captured at CFR SUBMISSION time inside SensingEngine::submit() -- i.e. AFTER producer/decoder latency, not at hardware acquisition time",
    "citations": [$SUBMIT_DEF_CITATION, $UTC_NS_ASSIGN_CITATION, $UTC_NS_FIELD_CITATION, $REPORT_JSON_UTC_CITATION]
  },
  "first_row_time_ns_last_row_time_ns_cpi_duration_ns": {
    "unit": "nanoseconds, RELATIVE (not an absolute epoch)",
    "clock_source": "slot/sample clock: CfrWindow.row_time_slots (absolute unwrapped slot count) times slot_duration_s(scs_hz)*1e9 -- derived from the RF slot timeline, independent of host wall clock",
    "citations": [$ROW_TIME_CITATION]
  },
  "midpoint_air_time_s": {
    "unit": "seconds, RELATIVE to an arbitrary per-run air_origin_slots reference (not an absolute epoch, not wall clock)",
    "clock_source": "slot/sample clock: 0.5*(row_time_slots.front()+row_time_slots.back()) minus air_origin_slots, times slot_duration_s(scs_hz); this is the time axis actually consumed by the sync/clock trackers and local/global trackers downstream",
    "citations": [$MIDPOINT_CITATION]
  },
  "note": "Two distinct, non-interchangeable clock domains are emitted per CPI report: (1) a wall-clock UTC submission timestamp measured AFTER producer/decoder delay (cpi_start_time_utc_ns), and (2) slot-clock-derived relative timings actually used for physical CPI/tracking math (first/last_row_time_ns, cpi_duration_ns, midpoint_air_time_s). Matches plan section 2.3's retraction: 'Report UTC is acquisition UTC' is INCORRECT -- submission UTC includes worker latency."
},
"notes": {
  "tree_dirty_state_vs_plan_baseline": "adaptive_RX_pipeline.md/adaptive_RX_pipeline_progress.md (written 2026-09-10) record exactly two pre-existing dirty items: 'M tests/passive_rx/run_adaptive_receive_test.sh' and '?? tests/passive_rx/aoa_track_dl.conf'. The MEASURED git status at manifest generation time (git.status_short above) shows additional modified/untracked files beyond those two (further M's under executables/, openair1/PHY/NR_UE_TRANSPORT/, radio/USRP/, tests/passive_rx/monitor/, and further ?? files including tests/passive_rx/adaptive_manual_dlul.conf, MANUAL_DL_UL.md, run_manual_x410.sh). This generator does not reset/clean/stash/touch any of them -- it only reads and records the CURRENT state, and writes its own new files under tests/passive_rx/baselines/. The discrepancy vs the plan's recorded baseline is reported here as a measured fact, not resolved."
}
}
JSONDOC

python3 -m json.tool "$MANIFEST.tmp" > "$MANIFEST"
rm -f "$MANIFEST.tmp"
echo "Wrote $MANIFEST"
echo "Wrote $PATCH (sha256=$PATCH_SHA)"
