#!/bin/sh
# Regenerate the LVGL fonts in main/fonts from the Alef TTFs in fonts_src/.
# Requires Node.js (npx downloads lv_font_conv on first use). Run from the repo root.
set -e
R="0x20-0x7E,0x590-0x5FF,0xFB1D-0xFB4F"   # ASCII + Hebrew (+ presentation forms)
for sz in 22 32 48; do
  npx --yes lv_font_conv@1.5.2 --font fonts_src/Alef-Regular.ttf -r "$R" --size $sz \
      --format lvgl --bpp 4 --no-compress --lv-include lvgl.h -o main/fonts/font_heb_$sz.c
done
npx --yes lv_font_conv@1.5.2 --font fonts_src/Alef-Bold.ttf -r 0x20-0x3A,0x41-0x5A,0x61-0x7A --size 80 \
    --format lvgl --bpp 4 --no-compress --lv-include lvgl.h -o main/fonts/font_clock_80.c
