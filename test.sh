#!/bin/bash
set -e

cd /home/dragonos/Sources/5G8atv-rf-hackrf-decoder

# Build (normal + WebGUI)
if [ -n "$1" ] && [ "$1" = "webgui" ]; then
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DWEBGUI=ON && cmake --build build -j4
elif [ -n "$1" ] && [ "$1" = "soapysdr" ]; then
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON && cmake --build build -j4
else
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
fi

echo "=== Golden test (30 fields) ==="
./build/synth_fm

echo "=== Long-run test (1200 fields) ==="
./build/synth_fm --fields 1200 --check-frames

echo "=== Generate test IQ for fpvdec replay ==="
./build/synth_fm --fields 1200 /tmp/bars.cs8

echo "=== fpvdec replay (headless dump) ==="
./build/fpvdec --input file --file /tmp/bars.cs8 --dump-frames out_ --frames 100

echo "=== Delete test IQ for fpvdec replay ==="
rm /tmp/bars.cs8

echo "=== Done ==="
