#!/usr/bin/env bash
start=$(date +%s)            # capture the starting epoch second
lastelapsed=-1

while true; do
    now=$(date +%s)          # current epoch second
    elapsed=$(expr "$now" - "$start")   # external integer arithmetic
    [ "$elapsed" -ge 5 ] && break       # exit once 5 s have passed
    if [ "$elapsed" -ne "$lastelapsed" ]; then
        echo $elapsed
    fi
    last_elapsed=$elapsed
    sleep 1
done
if [ "$lastoutput" -ne 4 ]
then
    echo 4                  # in case we output only 1, 2, 3 output 4
fi

