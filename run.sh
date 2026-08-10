#!/bin/bash

cd /home/dragonos/Sources/5G8atv-rf-hackrf-decoder

#cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON && cmake --build build -j4

#./build/fpvdec --no-amp --vga 34 --rate 10e6 --offset -160000 --gain auto --channel A1
#./build/fpvdec --no-amp --vga 34 --rate 10e6 --gain auto --freq 5865000000
#./build/fpvdec --no-amp --vga 36 --rate 10e6 --gain auto --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 --no-stats --no-clkin --no-agc --no-signal --auto-res
#./build/fpvdec --no-amp --vga 16 --rate 10e6 --gain manual --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 --no-stats --no-clkin --no-agc --no-signal --auto-res --source soapysdr --device "driver=uhd"
#./build/fpvdec --no-amp --vga 16 --rate 10e6 --gain manual --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 --no-stats --no-clkin --no-agc --no-signal --resolution 640x480 --source hackrf

#./build/fpvdec --no-amp --vga 36 --rate 9e6 --gain manual --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 192,192,0 --no-stats --no-clkin --no-agc --no-signal --resolution 640x480 --source hackrf --gui imgui --lpf 2e6 --dev 1e6 --no-afc --mode gray
./build/fpvdec --no-amp --vga 36 --rate 9e6 --gain manual --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 192,192,0 --no-stats --no-clkin --no-agc --no-signal --resolution 640x480 --source hackrf --gui imgui --lpf 2e6 --dev 1e6 --no-afc --mode gray --denoise-temporal 0.1 --denoise-temporal-median 7 --denoise-temporal-median-strength 0.5
