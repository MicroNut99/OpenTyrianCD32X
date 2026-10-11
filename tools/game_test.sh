#!/bin/bash
# Tyrian 32X - scripted real game on the PC (needs a file system with tyrian1.lvl):
#   title -> 1 player -> episode 1 -> difficulty -> shop -> Ship Specs -> back ->
#   Play Next Level -> level 1 with some moving and firing -> (dies) -> back to the title.
# The run is deterministic (fixed random seed), so two runs give identical frames.
#   tools/game_test.sh <romfs or .32x> <frames dir>
P="t11000:A,t11300:-,t14000:A,t14300:-,t17000:A,t17300:-,t20000:A,t20300:-"
P="$P,t26000:DOWN,t26300:-,t28000:A,t28300:-,t34000:A,t34300:-"
# the 32X game menu has "Play Next Level" first (port: places 2 and 6 swapped): from Ship Specs, one UP
P="$P,t38000:UP,t38300:-,t43000:A,t43300:-,t47000:A,t47300:-"
P="$P,t60000:A+UP,t66000:A+LEFT,t70000:A+RIGHT,t74000:-,t80000:START,t80300:-,t86000:C,t86300:-,t90000:A+UP,t100000:-"
mkdir -p "$2"
T32X_ROMFS="$1" T32X_FRAMES_DIR="$2" T32X_SAVE_EVERY=${T32X_SAVE_EVERY:-10} T32X_MAX_SECONDS=110 T32X_MAX_FRAMES=999999 \
T32X_PAD="$P" "${T32X_HOST:-$(dirname "$0")/../host/tyrian32x_host}"
