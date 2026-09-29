#!/usr/bin/env bash
git config --global --add safe.directory /__w/Variable-Trace/Variable-Trace

rm -rf build_test
mkdir build_test 
cd build_test
cmake .. -DMAKE_TESTS=1
make -j
cd ../build_test
./test/Variable-Trace_test

