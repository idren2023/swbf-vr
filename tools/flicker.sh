#!/bin/sh
# Flicker measure for a clip (runs 860-872 method): mean luma difference between consecutive frames
# (ffmpeg tblend difference + signalstats YAVG); prints frames, how many jump over 1.0, and the largest.
# Usage: sh tools/flicker.sh re/clips/<name>.mp4
ffmpeg -v error -i "$1" -vf "tblend=all_mode=difference,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-" -f null - |
  sed -n 's/.*YAVG=//p' | awk '{n++; if ($1>1.0) j++; if ($1>m) m=$1} END {printf "frames %d, over 1.0: %d, max %.2f\n", n, j, m}'
