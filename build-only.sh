#!/bin/bash

cd /home/dragonos/Sources/5G8atv-rf-hackrf-decoder

echo "=== Compile (ie: all, webgui, soapysdr, or default) ==="
# Build (normal + WebGUI)
if [ -n "$1" ] && [ "$1" = "webgui" ]; then
    echo "=== Compiling with WebUI (webgui) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DWEBGUI=ON && cmake --build build -j4
elif [ -n "$1" ] && [ "$1" = "soapysdr" ]; then
    echo "=== Compiling with SoapySDR (soapysdr) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON && cmake --build build -j4
elif [ -n "$1" ] && [ "$1" = "all" ]; then
    echo "=== Compiling with SoapySDR and WebUI (all) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON -DWEBGUI=ON && cmake --build build -j4
else
    echo "=== Compiling without SoapySDR nor WebUI support (default) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
fi
