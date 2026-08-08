#!/bin/bash

cd /home/dragonos/Sources/5G8atv-rf-hackrf-decoder

cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

#./build/fpvdec --no-amp --vga 34 --rate 10e6 --offset -160000 --gain auto --channel A1
#./build/fpvdec --no-amp --vga 34 --rate 10e6 --gain auto --freq 5865000000
./build/fpvdec --no-amp --vga 36 --rate 10e6 --gain auto --freq 5865000000 --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 --no-stats --no-clkin --no-agc --no-signal --auto-res
