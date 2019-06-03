#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: runtest.sh path/to/test.65c"
    exit 1
fi

set -e

SRC="$1"
OBJ="$SRC".o
LINK="$SRC".link
BIN="$SRC".bin
HWOUT="$SRC".hw.json
SWOUT="$SRC".sw.txt
JOB="$SRC".job.json
JOBTMPL="$SRC".job.tmpl

if [ ! -f "$BIN" -o "$SRC" -nt "$BIN" ]; then
    wla-65c02 -q -o "$OBJ" "$SRC"
    cat > "$LINK" <<EOF
[objects]
$OBJ
EOF
    wlalink "$LINK" "$BIN"
fi

if [ ! -f "$JOBTMPL" ]; then
    JOBTMPL=tests/standard.job.tmpl
fi

if [ ! -f "$JOB" -o "$SRC" -nt "$JOB" -o "$JOBTMPL" -nt "$JOB" ]; then
    util/makejob.lua "$JOBTMPL" "$BIN" "$JOB"
fi

if [ ! -f "$HWOUT" -o "$JOB" -nt "$HWOUT" ]; then
    wget -O "$HWOUT" -q --post-file "$JOB" http://localhost/65test.cgi
fi
