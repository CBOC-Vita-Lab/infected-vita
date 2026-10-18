#!/bin/bash
# native (x86) build of the PortableKit runtime + interpreter objects for the differential test; one core, nice'd.
set -e
R=/home/claude/assess/PortableKit
O=/home/claude/assess/experiments/diff
cd $O
for f in runtime interpreter nid_registry; do
  [ -f $f.o ] || nice -n 10 g++ -std=c++20 -O1 -I$R/include -c $R/src/$f.cpp -o $f.o
done
echo rt-built
