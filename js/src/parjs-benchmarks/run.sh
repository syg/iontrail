#!/bin/bash

if [[ -z "$1" ]] || [[ -z "$2" ]]; then
    echo "Usage: run.sh path-to-shell path-to-test"
fi

D="$(dirname $0)"
S="$1"
shift
for T in "$@"; do
    echo "$S" -e "'"'var libdir="'$D'/";'"'" "$T"
    "$S" -e 'var libdir="'$D'/";' "$T"
done