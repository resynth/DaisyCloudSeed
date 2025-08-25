#!/bin/bash
start_dir=$PWD

echo $start_dir

echo "rebuilding everything. . . "
echo "only errors, and warnings will output. . . "
echo "-------------------"
sleep 1


echo "rebuilding libcloudseed"
cd CloudSeed
make clean
make
echo "done building libcloudseed"
cd "$start_dir"

echo "rebuilding libdaisy"
cd libdaisy
make clean
make
echo "done building libdaisy"

echo "rebuilding DaisySP"
cd "$start_dir"
cd DaisySP
make clean
make
cd "$start_dir"
echo "done building daisySP"
echo "done building libs"

