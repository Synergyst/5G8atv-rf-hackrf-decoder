#!/bin/bash

cd /home/dragonos/Sources/5G8atv-rf-hackrf-decoder

#cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSOAPYSDR=ON && cmake --build build -j4

#./build/fpvdec --no-amp --vga 34 --rate 10e6 --offset -160000 --gain auto --channel A1
#./build/fpvdec --no-amp --vga 34 --rate 10e6 --gain auto --freq 5865000000
#./build/fpvdec --no-amp --vga 36 --rate 10e6 --gain auto --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 --no-stats --no-clkin --no-agc --no-signal --auto-res
#./build/fpvdec --no-amp --vga 16 --rate 10e6 --gain manual --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 --no-stats --no-clkin --no-agc --no-signal --auto-res --source soapysdr --device "driver=uhd"
./build/fpvdec --no-amp --vga 16 --rate 10e6 --gain manual --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 --no-stats --no-clkin --no-agc --no-signal --resolution 640x480 --source hackrf
