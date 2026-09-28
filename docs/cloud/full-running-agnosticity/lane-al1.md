# Lane override — READ AFTER context.md; this REPLACES its R and B

You work in lane **al1**, a dedicated git worktree on sens6, so other agents can work in parallel:
- `R=/home/sens/NICOLA/agn-wt/al1`  (branch `sdd/agn-al1`). Edit and commit ONLY here.
- `B=$R/cmake_targets/ran_build/build` (already configured, starts empty: the first build compiles from scratch — expect 10-30 min for nr-uesoftmodem).
- Build ONLY with: `ssh sens6 "/home/sens/NICOLA/agn-wt/lane-make.sh al1 <targets...>"` (4 jobs, shared ccache). Never run make in another directory, never -j12.
- Run tests from $B: `ssh sens6 "cd /home/sens/NICOLA/agn-wt/al1/cmake_targets/ran_build/build && ctest -R <name> --output-on-failure"`.
- Wherever your brief says `/home/sens/NICOLA/adaptive-rx-UL-DL` or its build dir, use the lane paths above instead.
- Other lanes are editing other files concurrently; do not touch files outside your task.
- **Run cmake ONLY inside $B** (`cd $B && cmake .`). Never run cmake in $R: it overwrites tracked Makefiles in the source tree (this already happened once).
