#!/usr/bin/env python3
"""Build the recorded-input diagnostic with this worktree's CMake flags.
Invoke via heavy.sh; receiver build and minimal_lib must already exist.
"""
import pathlib, shlex, subprocess
root = pathlib.Path(__file__).resolve().parents[2]
b = root / "cmake_targets/ran_build/build"
commands = subprocess.check_output(["ninja", "-C", str(b), "-t", "commands", "test_ldpc_cb0_cuda"], text=True).splitlines()
src = root / "tests/passive_rx/decodefix_ldpc.c"
obj = b / "decodefix_ldpc.o"
c = next(shlex.split(x) for x in commands if " -c " in x and "ldpc_cuda_pool_test_helper.c" in x)
c[c.index("-o")+1] = str(obj)
c[c.index("-c")+1] = str(src)
if "-MF" in c: c[c.index("-MF")+1] = str(obj)+".d"
subprocess.run(c, cwd=b, check=True)
l = shlex.split(commands[-1])[2:-2]
l = [x for x in l if "libgtest" not in x and not (x.endswith(".o") and "crc_byte" not in x)]
l[l.index("-o")+1] = str(b / "decodefix_ldpc")
l[1:1] = [str(obj), "CMakeFiles/PHY_NR_COMMON.dir/openair1/PHY/CODING/nr_segmentation.c.o", "CMakeFiles/PHY_NR_COMMON.dir/openair1/PHY/NR_TRANSPORT/nr_tbs_tools.c.o"]
subprocess.run(l, cwd=b, check=True)
