#!/bin/bash
echo "rebuilding examples:"
start_dir=$PWD
HW=hardware_platforms
example_dirs=( patch )
for e in ${example_dirs[@]}; do
    for d in $e/*/; do
        echo "rebuilding $d"
        cd "$d"
        make clean
        make
        cd "$start_dir"
        echo "done"
    done
done
