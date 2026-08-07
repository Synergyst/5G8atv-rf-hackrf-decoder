#!/bin/bash

cd /home/dragonos/Sources/5G8atv-rf-hackrf-decoder

#./build/fpvdec --no-amp --vga 34 --rate 10e6 --offset -160000 --gain auto --channel A1
./build/fpvdec --no-amp --vga 34 --rate 10e6 --gain auto --freq 5865000000
