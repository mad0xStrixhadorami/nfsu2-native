#!/bin/bash
S=/tmp/claude-1000/-home-kali/423ae27c-5b5b-47c1-80ee-326462b0715a/scratchpad
cd ~/re/nfsu2_decomp
tag=$1; orbit=$2; ms=${3:-19000}
NFSU2_FORCE_FLAMES=1 NFSU2_NO_INTRO=1 NFSU2_NOINPUT=1 NFSU2_WORLD=L4RA NFSU2_RACETYPE=9 NFSU2_NIGHT=1 NFSU2_DRIVE="14000-60000:up" NFSU2_CAM_ORBIT=$orbit NFSU2_SCREENSHOT_MS=$ms NFSU2_SCREENSHOT=$S/fl_$tag.bmp NFSU2_DATA=~/re/nfsu2_game timeout 120 ./build/nfsu2 >/dev/null 2>&1
python3 -c "
from PIL import Image
Image.open('$S/fl_$tag.bmp').convert('RGB').save('$S/fl_$tag.png')"
