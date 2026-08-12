#!/bin/bash
set -e

cd "$(dirname "$(readlink -f "$0")")"

echo "=== Compile (ie: all, uhd, webgui, soapysdr, or default) ==="
if [ -n "$1" ] && [ "$1" = "webgui" ]; then
    echo "=== Compiling with WebUI (webgui) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DWEBGUI=ON && cmake --build build -j4
elif [ -n "$1" ] && [ "$1" = "soapysdr" ]; then
    echo "=== Compiling with SoapySDR (soapysdr) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON && cmake --build build -j4
elif [ -n "$1" ] && [ "$1" = "uhd" ]; then
    echo "=== Compiling with native UHD (uhd) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DUHD=ON && cmake --build build -j4
elif [ -n "$1" ] && [ "$1" = "all" ]; then
    echo "=== Compiling with SoapySDR, native UHD, and WebUI (all) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON -DUHD=ON -DWEBGUI=ON && cmake --build build -j4
else
    echo "=== Compiling without SoapySDR, UHD, or WebUI support (default) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
fi

echo "=== Golden test (30 fields) ==="
./build/synth_fm

echo "=== Long-run test (1200 fields) ==="
./build/synth_fm --fields 100 --check-frames

echo "=== Generate test IQ for fpvdec replay ==="
./build/synth_fm --fields 1200 /tmp/bars.cs8

echo "=== fpvdec replay (headless dump) ==="
./build/fpvdec --input file --file /tmp/bars.cs8 --dump-frames out_ --frames 100

echo "=== Delete test IQ for fpvdec replay ==="
rm /tmp/bars.cs8
echo "=== Done ==="
