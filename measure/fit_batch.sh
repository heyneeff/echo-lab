#!/bin/bash
# fit a list of styles, two at a time
cd "$(dirname "$0")/.."
run() { FIT_POLES=auto FIT_EVALS=120 nice -n 15 ~/Developer/listen/.venv/bin/python measure/fit.py "$1" > "measure/fits/$1.log" 2>&1; echo "done $1: $(tail -1 "measure/fits/$1.log")"; }
(run "Studio Tape"; run "Memory Man"; run "Digital Delay") &
(run "EchoPlex"; run "DM-2"; run "AM Radio") &
wait
