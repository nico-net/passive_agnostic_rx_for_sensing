#!/usr/bin/env python3
"""P01 manifest/acceptance/geometry checker. stdlib only. Read-only: never builds, never runs
the radio, never touches tests/passive_rx/run_adaptive_receive_test.sh.

Usage:
  check_manifest.py MANIFEST.json            recompute every identity/hash it records against the
                                              live tree/build; exit non-zero listing each mismatch.
  check_manifest.py --acceptance ACC.json --geometry GEO.json
                                              exit non-zero if any deployment-dependent limit is
                                              UNSET or the geometry is not surveyed.
  check_manifest.py --fixtures FIXTURES.json exit non-zero if admissible_fixtures is empty, or if
                                              any fixture's replay.bin/receiver.conf/run.log sha256,
                                              producing binary sha256, or full_auto value no longer
                                              match the live tree/build/filesystem.
  check_manifest.py --selftest               self-check: (a) fresh manifest passes, (b) a copy
                                              with one binary hash altered fails, (c) the shipped
                                              acceptance/geometry files fail, (d) a copy whose
                                              selected_test_conf full_auto=1 fails, (e) a copy with
                                              one citation's line range shifted fails, (f) a
                                              synthetic single-fixture registry passes, an altered
                                              hash in it fails, and the shipped P02 fixtures.json
                                              passes (it carries >=1 registered admissible fixture
                                              as of fix round 2).
"""
import glob
import hashlib
import json
import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, cwd=REPO):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=False, check=True).stdout


def run_text(cmd, cwd=REPO):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=True).stdout


def cache_get(cache_path, key):
    with open(cache_path) as f:
        for line in f:
            if line.startswith(key + ":"):
                return line.rstrip("\n").split("=", 1)[1]
    return None


_CITATION_KEYS = {"file", "line_start", "line_end", "token"}


def find_citations(obj, path=""):
    """Yield (json_path, citation_dict) for every {file,line_start,line_end,token} object
    reachable inside obj (the generator's structured citations, wherever they're nested)."""
    if isinstance(obj, dict):
        if set(obj.keys()) == _CITATION_KEYS:
            yield path, obj
            return
        for k, v in obj.items():
            yield from find_citations(v, f"{path}.{k}" if path else k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from find_citations(v, f"{path}[{i}]")


def check_citations(manifest_obj):
    """Re-verify every structured citation's `token` still occurs within its recorded
    [line_start, line_end] range of the LIVE file. Returns a list of mismatch strings."""
    mismatches = []
    for path, cit in find_citations(manifest_obj):
        rel = cit.get("file")
        full = os.path.join(REPO, rel) if rel else None
        if not full or not os.path.isfile(full):
            mismatches.append(f"citation {path}: file does not exist on the live tree: {rel!r}")
            continue
        ls, le, token = cit.get("line_start"), cit.get("line_end"), cit.get("token")
        try:
            with open(full, errors="replace") as f:
                lines = f.readlines()
        except OSError as e:
            mismatches.append(f"citation {path}: could not read {rel!r}: {e}")
            continue
        found = False
        if isinstance(ls, int) and isinstance(le, int) and ls >= 1 and le >= ls:
            for ln in range(ls, le + 1):
                if 1 <= ln <= len(lines) and token in lines[ln - 1]:
                    found = True
                    break
        if not found:
            mismatches.append(
                f"citation {path}: token {token!r} not found within {rel}:{ls}-{le} on the live tree")
    return mismatches


def conf_full_auto(conf_path):
    if not os.path.isfile(conf_path):
        return None
    with open(conf_path) as f:
        text = f.read()
    import re
    m = re.search(r"pdcch_blind_monitor_full_auto\s*=\s*([0-9]+)", text)
    return int(m.group(1)) if m else 0  # absent => parser default 0


def check_identity(manifest_path):
    """Recompute every hash/identity the manifest records against the live tree/build.
    Returns a list of mismatch strings (empty = all match)."""
    with open(manifest_path) as f:
        m = json.load(f)
    mismatches = []

    def cmp(label, recorded, live):
        if recorded != live:
            mismatches.append(f"{label}: manifest={recorded!r} live={live!r}")

    build = os.path.join(REPO, "cmake_targets", "ran_build", "build")

    # ---- git identity ----
    live_branch = run_text(["git", "branch", "--show-current"]).strip()
    live_commit = run_text(["git", "rev-parse", "HEAD"]).strip()
    cmp("git.branch", m["git"]["branch"], live_branch)
    cmp("git.commit", m["git"]["commit"], live_commit)
    live_diff = run(["git", "diff", "--binary"])
    live_diff_sha = hashlib.sha256(live_diff).hexdigest()
    cmp("git.tracked_diff_patch_sha256 (recomputed live git diff --binary)",
        m["git"]["tracked_diff_patch_sha256"], live_diff_sha)

    # ---- scoped files (same ls-files set the launcher hashes) ----
    out = run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--",
               "openair1", "openair2", "executables", "radio", "tests/passive_rx"])
    live_paths = [p for p in out.decode().split("\0") if p]
    live_hashes = {}
    for p in live_paths:
        full = os.path.join(REPO, p)
        if os.path.isfile(full):
            live_hashes[p] = sha256_file(full)
    recorded_hashes = {e["path"]: e["sha256"] for e in m["git"]["scoped_files"]}
    for path, rsha in recorded_hashes.items():
        if path not in live_hashes:
            mismatches.append(f"scoped_file missing now: {path}")
        elif live_hashes[path] != rsha:
            mismatches.append(f"scoped_file changed: {path} manifest={rsha} live={live_hashes[path]}")
    new_files = [p for p in live_hashes if p not in recorded_hashes]
    if new_files:
        # Informational, not a mismatch: a file appearing AFTER the manifest was generated does
        # not contradict anything the manifest recorded. A file it DID record going missing or
        # changing hash (above) is the actual identity break this function exists to catch.
        print(f"NOTE: {len(new_files)} scoped file(s) exist now but were not present at manifest "
              f"generation time (not scored as a mismatch): "
              + ", ".join(sorted(new_files)[:10]) + (" ..." if len(new_files) > 10 else ""),
              file=sys.stderr)

    # ---- build/driver identity ----
    bin_path = os.path.join(build, "nr-uesoftmodem")
    drv_path = os.path.join(build, "liboai_usrpdevif.so")
    cmp("build.nr_uesoftmodem.sha256", m["build"]["nr_uesoftmodem"]["sha256"], sha256_file(bin_path))
    cmp("build.liboai_usrpdevif.sha256", m["build"]["liboai_usrpdevif"]["sha256"], sha256_file(drv_path))
    device_so = os.path.join(build, "liboai_device.so")
    try:
        live_symlink = os.readlink(device_so)
        cmp("build.liboai_device_so_symlink_target", m["build"]["liboai_device_so_symlink_target"], live_symlink)
    except OSError as e:
        mismatches.append(f"build.liboai_device_so_symlink_target: could not readlink {device_so}: {e}")

    # ---- cmake cache ----
    cache_path = os.path.join(build, "CMakeCache.txt")
    for key in ("CMAKE_BUILD_TYPE", "CMAKE_GENERATOR", "ENABLE_ISAC_SENSING", "ENABLE_TESTS",
                "ENABLE_CHANNEL_SIM_CUDA", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"):
        cmp(f"cmake_cache.{key}", m["cmake_cache"][key], cache_get(cache_path, key))
    cc = m["cmake_cache"]["CMAKE_C_COMPILER"]
    cxx = m["cmake_cache"]["CMAKE_CXX_COMPILER"]
    if os.path.isfile(cc):
        cmp("cmake_cache.CMAKE_C_COMPILER_VERSION", m["cmake_cache"]["CMAKE_C_COMPILER_VERSION"],
            run_text([cc, "--version"]).splitlines()[0])
    if os.path.isfile(cxx):
        cmp("cmake_cache.CMAKE_CXX_COMPILER_VERSION", m["cmake_cache"]["CMAKE_CXX_COMPILER_VERSION"],
            run_text([cxx, "--version"]).splitlines()[0])

    # ---- configuration cases: recompute conf sha256 + full_auto for every non-null candidate ----
    cases = m["configuration_cases"]
    for case_name, case in cases.items():
        candidates = case.get("candidates") if isinstance(case, dict) else None
        entries = []
        if candidates:
            entries = list(candidates.values())
        elif isinstance(case, dict) and case.get("conf"):
            entries = [case]
        for entry in entries:
            conf = entry.get("conf")
            if not conf:
                continue
            full_path = os.path.join(REPO, conf)
            if not os.path.isfile(full_path):
                mismatches.append(f"configuration_cases[{case_name}]: conf missing now: {conf}")
                continue
            cmp(f"configuration_cases[{case_name}][{conf}].sha256", entry["sha256"], sha256_file(full_path))
            recorded_fa = entry["measured"]["pdcch_blind_monitor_full_auto"]
            cmp(f"configuration_cases[{case_name}][{conf}].measured.pdcch_blind_monitor_full_auto",
                recorded_fa, conf_full_auto(full_path))

    # ---- selected_test_conf gate: TESTING MODE RULE (plan section 1, 2026-09-11 amendment) ----
    sel = m.get("selected_test_conf", {})
    sel_fa = sel.get("pdcch_blind_monitor_full_auto")
    if sel_fa != 0:
        mismatches.append(
            f"selected_test_conf.pdcch_blind_monitor_full_auto = {sel_fa!r} (must be 0 -- "
            f"manual mode only, plan section 1 TESTING MODE RULE; a full_auto=1 conf is VOID "
            f"for every gate in the plan)")
    elif sel.get("conf"):
        live_fa = conf_full_auto(os.path.join(REPO, sel["conf"]))
        if live_fa != 0:
            mismatches.append(
                f"selected_test_conf {sel['conf']!r} now measures full_auto={live_fa} on the live "
                f"tree (manifest recorded 0)")

    # ---- every file:line citation in the manifest (antenna_mapping_and_geometry,
    # timestamp_units, rank_two_array_check, ...): re-verify its token is still where it says. ----
    mismatches.extend(check_citations(m))

    return mismatches


def check_profile(acceptance_path, geometry_path):
    """Return a list of reasons the acceptance/geometry pair is incomplete (empty = complete)."""
    reasons = []
    with open(acceptance_path) as f:
        acc = json.load(f)
    with open(geometry_path) as f:
        geo = json.load(f)

    if acc.get("exact_invariants", {}).get("full_auto_must_be_zero", {}).get("value") is not True:
        reasons.append("acceptance: exact_invariants.full_auto_must_be_zero is not true")

    for key, entry in acc.get("deployment_dependent_limits", {}).items():
        if entry.get("status") != "SET":
            reasons.append(f"acceptance: deployment_dependent_limits.{key} status={entry.get('status')!r} (must be SET)")
        if entry.get("status") == "UNSET" and entry.get("value") is not None:
            reasons.append(f"acceptance: deployment_dependent_limits.{key} is UNSET but has a non-null value (invented favorable limit?)")

    if geo.get("surveyed") is not True:
        reasons.append(f"geometry: surveyed={geo.get('surveyed')!r} (must be true)")

    return reasons


def check_fixtures(fixtures_path):
    """P02: verify a fixtures.json registry of admissible baseline replay fixtures.
    Returns a list of mismatch/incompleteness reasons (empty = all admissible fixtures verified).
    An EMPTY admissible_fixtures list is itself a failing reason -- G0 requires at least one
    reproducible supported DL and UL case, and a registry recording zero is honest, not silent."""
    with open(fixtures_path) as f:
        data = json.load(f)
    reasons = []
    admissible = data.get("admissible_fixtures", [])
    if not admissible:
        reasons.append(
            "fixtures.json: admissible_fixtures is empty -- no reproducible baseline replay "
            "fixture is currently registered (see the file's status/blocker fields)")
        return reasons

    build = os.path.join(REPO, "cmake_targets", "ran_build", "build")
    try:
        live_bin_sha = sha256_file(os.path.join(build, "nr-uesoftmodem"))
    except OSError as e:
        reasons.append(f"could not hash live nr-uesoftmodem binary: {e}")
        live_bin_sha = None

    for i, fx in enumerate(admissible):
        label = f"admissible_fixtures[{i}] ({fx.get('path', '?')})"
        fdir = fx.get("path")
        for fname, key in (("replay.bin", "replay_bin_sha256"),
                           ("receiver.conf", "receiver_conf_sha256"),
                           ("run.log", "run_log_sha256")):
            recorded = fx.get(key)
            if not recorded:
                reasons.append(f"{label}: missing recorded {key}")
                continue
            full = os.path.join(fdir, fname) if fdir else None
            try:
                live = sha256_file(full) if full else None
            except OSError as e:
                reasons.append(f"{label}: could not hash {fname} at {full!r}: {e}")
                continue
            if live != recorded:
                reasons.append(f"{label}: {fname} sha256 registered={recorded} live={live}")

        recorded_bin = fx.get("producing_binary_sha256")
        if recorded_bin != live_bin_sha:
            reasons.append(
                f"{label}: producing_binary_sha256 registered={recorded_bin!r} "
                f"live_current_build={live_bin_sha!r}")

        fa = fx.get("full_auto")
        if fa != 0:
            reasons.append(f"{label}: full_auto={fa!r} (must be 0 -- TESTING MODE RULE)")

    return reasons


def _mutate_json(src_path, mutate_fn):
    with open(src_path) as f:
        data = json.load(f)
    mutate_fn(data)
    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as f:
        json.dump(data, f)
    return path


def selftest():
    manifests = sorted(
        glob.glob(os.path.join(SCRIPT_DIR, "baselines", "manifest_*.json")),
        key=os.path.getmtime,
    )
    manifests = [p for p in manifests if not p.endswith("_tracked.patch")]
    if not manifests:
        print("SELFTEST: no manifest found under tests/passive_rx/baselines/ -- run "
              "baseline_manifest.sh first")
        return 1

    latest = manifests[-1]
    ok = True

    # (a) fresh manifest passes
    mism = check_identity(latest)
    passed_a = not mism
    print(f"(a) fresh manifest ({os.path.basename(latest)}) expected PASS: "
          f"{'PASS' if passed_a else 'FAIL'}")
    if not passed_a:
        ok = False
        for line in mism:
            print("    " + line)

    # (b) a copy with one binary hash altered must FAIL
    bad_hash_path = _mutate_json(
        latest, lambda d: d["build"]["nr_uesoftmodem"].__setitem__("sha256", "0" * 64))
    mism_b = check_identity(bad_hash_path)
    passed_b = bool(mism_b)
    print(f"(b) manifest with altered nr_uesoftmodem.sha256 expected FAIL: "
          f"{'PASS' if passed_b else 'FAIL (checker did not detect the alteration)'}")
    if not passed_b:
        ok = False
    os.unlink(bad_hash_path)

    # (c) the shipped acceptance/geometry files must FAIL (unset by construction)
    acc_path = os.path.join(SCRIPT_DIR, "acceptance_four_rx.json")
    geo_path = os.path.join(SCRIPT_DIR, "geometry_four_rx.json")
    reasons_c = check_profile(acc_path, geo_path)
    passed_c = bool(reasons_c)
    print(f"(c) shipped acceptance/geometry files expected FAIL (unset/unsurveyed): "
          f"{'PASS' if passed_c else 'FAIL (checker accepted an incomplete profile)'}")
    if not passed_c:
        ok = False
    else:
        for line in reasons_c:
            print("    " + line)

    # (d) amendment: selected_test_conf with full_auto=1 must FAIL; full_auto=0 must PASS (case a
    # already covers the pass side on the real manifest's own selected_test_conf).
    bad_mode_path = _mutate_json(
        latest,
        lambda d: d["selected_test_conf"].update(
            {"conf": "tests/passive_rx/adaptive_no_hints.conf", "pdcch_blind_monitor_full_auto": 1}),
    )
    mism_d = check_identity(bad_mode_path)
    passed_d = bool(mism_d) and any("full_auto" in line for line in mism_d)
    print(f"(d) manifest with selected_test_conf full_auto=1 (adaptive_no_hints.conf) expected FAIL: "
          f"{'PASS' if passed_d else 'FAIL (checker did not reject full_auto=1)'}")
    if not passed_d:
        ok = False
    os.unlink(bad_mode_path)

    # (e) a copy with one citation's line range shifted off its real token must FAIL
    def _shift_citation(d):
        cit = d["antenna_mapping_and_geometry"]["known_from_source"]["usrp_args_citation"]
        cit["line_start"] += 500
        cit["line_end"] += 500

    bad_citation_path = _mutate_json(latest, _shift_citation)
    mism_e = check_identity(bad_citation_path)
    passed_e = bool(mism_e) and any("citation" in line for line in mism_e)
    print(f"(e) manifest with a shifted citation line range expected FAIL: "
          f"{'PASS' if passed_e else 'FAIL (checker did not detect the shifted citation)'}")
    if not passed_e:
        ok = False
    os.unlink(bad_citation_path)

    # (f) P02 fixtures.json checker: a synthetic single-fixture registry must PASS, an altered
    # hash in a copy of it must FAIL, and the real shipped (currently empty) fixtures.json must
    # FAIL too -- mirrors (c)'s "shipped incomplete profile is honestly rejected" pattern.
    build = os.path.join(REPO, "cmake_targets", "ran_build", "build")
    try:
        live_bin_sha = sha256_file(os.path.join(build, "nr-uesoftmodem"))
    except OSError:
        live_bin_sha = None
    fx_dir = tempfile.mkdtemp(prefix="p02_fixture_selftest_")
    try:
        contents = {"replay.bin": b"synthetic replay bytes for selftest only",
                    "receiver.conf": b"pdcch_blind_monitor_full_auto = 0;\n",
                    "run.log": b"synthetic run.log for selftest only\n"}
        for fname, data in contents.items():
            with open(os.path.join(fx_dir, fname), "wb") as fh:
                fh.write(data)
        good_registry = {"admissible_fixtures": [{
            "path": fx_dir,
            "replay_bin_sha256": hashlib.sha256(contents["replay.bin"]).hexdigest(),
            "receiver_conf_sha256": hashlib.sha256(contents["receiver.conf"]).hexdigest(),
            "run_log_sha256": hashlib.sha256(contents["run.log"]).hexdigest(),
            "producing_binary_sha256": live_bin_sha,
            "full_auto": 0,
        }]}
        fd, good_path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as fh:
            json.dump(good_registry, fh)

        reasons_f1 = check_fixtures(good_path)
        passed_f1 = not reasons_f1
        print(f"(f1) synthetic single-fixture registry expected PASS: "
              f"{'PASS' if passed_f1 else 'FAIL'}")
        if not passed_f1:
            ok = False
            for line in reasons_f1:
                print("    " + line)
        os.unlink(good_path)

        bad_registry = json.loads(json.dumps(good_registry))
        bad_registry["admissible_fixtures"][0]["replay_bin_sha256"] = "0" * 64
        fd, bad_path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as fh:
            json.dump(bad_registry, fh)
        reasons_f2 = check_fixtures(bad_path)
        passed_f2 = bool(reasons_f2)
        print(f"(f2) synthetic registry with altered replay.bin hash expected FAIL: "
              f"{'PASS' if passed_f2 else 'FAIL (checker did not detect the alteration)'}")
        if not passed_f2:
            ok = False
        os.unlink(bad_path)
    finally:
        for fname in os.listdir(fx_dir):
            os.unlink(os.path.join(fx_dir, fname))
        os.rmdir(fx_dir)

    # P02 fix round 2: the shipped fixtures.json is now RESOLVED (>=1 admissible fixture, see
    # tests/passive_rx/baselines/fixtures.json's resolution_summary) -- this case therefore
    # expects PASS, not FAIL. Before the fix it legitimately expected FAIL (empty registry); the
    # assertion direction tracks the file's actual state rather than being pinned to history.
    shipped_fixtures_path = os.path.join(SCRIPT_DIR, "baselines", "fixtures.json")
    if os.path.isfile(shipped_fixtures_path):
        reasons_f3 = check_fixtures(shipped_fixtures_path)
        passed_f3 = not reasons_f3
        print(f"(f3) shipped fixtures.json expected PASS (>=1 admissible fixture registered): "
              f"{'PASS' if passed_f3 else 'FAIL'}")
        if not passed_f3:
            ok = False
            for line in reasons_f3:
                print("    " + line)
    else:
        print("(f3) shipped fixtures.json: SKIPPED (file not found)")
        ok = False

    print()
    print("SELFTEST OVERALL: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if "--fixtures" in argv:
        try:
            fx = argv[argv.index("--fixtures") + 1]
        except IndexError:
            print("usage: check_manifest.py --fixtures FIXTURES.json", file=sys.stderr)
            return 2
        reasons = check_fixtures(fx)
        if reasons:
            print(f"FIXTURES REJECTED ({len(reasons)}):")
            for r in reasons:
                print("  " + r)
            return 1
        print("FIXTURES OK: all registered admissible fixtures match the live tree/build/filesystem")
        return 0
    if "--acceptance" in argv or "--geometry" in argv:
        try:
            acc = argv[argv.index("--acceptance") + 1]
            geo = argv[argv.index("--geometry") + 1]
        except (ValueError, IndexError):
            print("usage: check_manifest.py --acceptance ACC.json --geometry GEO.json", file=sys.stderr)
            return 2
        reasons = check_profile(acc, geo)
        if reasons:
            print("ACCEPTANCE/GEOMETRY PROFILE INCOMPLETE:")
            for r in reasons:
                print("  " + r)
            return 1
        print("ACCEPTANCE/GEOMETRY PROFILE COMPLETE")
        return 0
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    mismatches = check_identity(argv[0])
    if mismatches:
        print(f"MANIFEST MISMATCH ({len(mismatches)}):")
        for line in mismatches:
            print("  " + line)
        return 1
    print("MANIFEST OK: all recorded identities/hashes match the live tree/build")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
