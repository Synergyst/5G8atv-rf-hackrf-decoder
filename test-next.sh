#!/bin/bash
set -e

cd "$(dirname "$(readlink -f "$0")")"

echo "=== Compile (ie: all, webgui, soapysdr, or default) ==="
# Build (normal + WebGUI)
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
    echo "=== Compiling with SoapySDR and WebUI (all) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON -DUHD=ON -DWEBGUI=ON && cmake --build build -j4
else
    echo "=== Compiling without SoapySDR nor WebUI support (default) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
fi

echo "=== Record IQ from SDR devices ==="
echo "=== Capturing HackRF IQ for 5 seconds ==="
./third_party/sdr_receiver_testing/build/sdr_app -o /tmp/hackrf.iq -m 1 -f 5866270000 -s 10e6 --lna 40 --vga 12 --amp 0 -d 5
echo "=== Capturing LibreSDR B220mini IQ for 5 seconds ==="
./third_party/sdr_receiver_testing/build/sdr_app -o /tmp/b220_sc8.iq -m 2 -f 5866270000 -s 10e6 --uhd-gain 50 -d 5

echo "=== Play IQ capture files using fpvdec ==="
echo "=== Playing HackRF IQ capture ==="
./build/fpvdec --input file --file /tmp/hackrf.iq --gui sdl --freq 5866270000 --resolution 640x480
echo "=== Playing LibreSDR B220mini IQ capture ==="
./build/fpvdec --input file --file /tmp/b220_sc8.iq --gui sdl --freq 5866270000 --resolution 640x480

echo "=== Delete test IQ capture files ==="
rm /tmp/b220_sc8.iq /tmp/hackrf.iq

echo "=== Done ==="
