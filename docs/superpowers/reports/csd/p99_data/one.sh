#!/bin/bash
source <(sed -n '/^S=/,/^}/p' drive.sh)
run "$@"
