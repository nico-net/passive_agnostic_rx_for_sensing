#!/bin/bash
# usage: pkg.sh BASE HEAD  -> writes review-BASE..HEAD.diff from the sens6 repo, prints path
W=/home/sens/NICOLA/docs/superpowers/sdd/full-running-agnosticity
o=$W/review-${1:0:7}..${2:0:7}.diff
ssh sens6 "REPO=$REPO; cd ${REPO:-/home/sens/NICOLA/adaptive-rx-UL-DL} && git merge-base --is-ancestor $1 $2 && echo '## commits' && git log --oneline $1..$2 && echo && echo '## stat' && git diff --stat $1..$2 && echo && echo '## diff' && git diff -U10 $1..$2" > "$o" || { echo "bad range" >&2; exit 3; }
echo "$o"
