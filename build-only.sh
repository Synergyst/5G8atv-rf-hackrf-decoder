#!/bin/bash

cd /home/dragonos/Sources/5G8atv-rf-hackrf-decoder

cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
