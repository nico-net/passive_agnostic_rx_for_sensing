#!/usr/bin/env python3
"""Hardware-free subprocess test of production RU reads, ITTI gate and shutdown."""
import os
import subprocess
import sys
import time

binary, mode = sys.argv[1:]
env = dict(os.environ, ISAC_SHUTDOWN_TIMEOUT_S="1")
env.pop("ISAC_RF_FAIL_AFTER_READS", None)
if mode == "injected":
    env["ISAC_RF_FAIL_AFTER_READS"] = "3"
start = time.monotonic()
with subprocess.Popen([binary, mode], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as child:
    # This binary has only a fake radio, never a receiver/UHD/rfsim process.
    try:
        output, _ = child.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        child.terminate()
        output, _ = child.communicate(timeout=2)
        raise AssertionError(f"fake receiver missed deadline: {output!r}")
elapsed = time.monotonic() - start
output = output.decode()
print(output, end="")
print(f"{mode}: status={child.returncode} elapsed={elapsed:.3f}s")
expected = 130 if mode in ("sigint", "repeated") else 143 if mode in ("sigterm", "active_read") else 1 if mode == "fatal_in_read" else 3
assert child.returncode == expected, (child.returncode, expected)
assert elapsed < 3, elapsed
assert output.count("FAKE_STREAM_STOP") == 1, output
if mode == "blocked_radio":
    assert "watchdog_exit_radio_release_incomplete" in output
    assert "FAKE_CLAIM_RELEASED" not in output
else:
    assert output.count("FAKE_CLAIM_RELEASED") == 1, output
    assert "radio_release_complete" in output
    if mode == "blocked_main":
        assert "watchdog_exit_radio_released" in output
    else:
        assert "watchdog" not in output, output
        if mode != "direct_exit":
            assert "step=complete" in output
