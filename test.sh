#!/bin/bash
set -e

cd /home/dragonos/Sources/5G8atv-rf-hackrf-decoder

# Build
if [ -n "$1" ]; then
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON && cmake --build build -j4
else
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
fi

echo "=== Golden test (30 fields) ==="
./build/synth_fm

echo "=== Long-run test (1200 fields) ==="
./build/synth_fm --fields 1200 --check-frames

echo "=== Generate test IQ for fpvdec replay ==="
./build/synth_fm --fields 1200 bars.cs8

echo "=== fpvdec replay (headless dump) ==="
./build/fpvdec --input file --file bars.cs8 --dump-frames out_ --frames 100

echo "=== Done ==="
