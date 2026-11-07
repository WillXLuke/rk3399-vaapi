#!/bin/bash
T=/home/durian/workspace/va-test/enc_ffmpeg_like
fails=0
for res in "640 480" "1280 720" "1600 900" "1920 1080"; do
 set -- $res; w=$1; h=$2
 for fps in 15 30 60; do
  for rc in CQP CBR VBR; do
   for ri in 0 50; do
    out=$(timeout 90 $T $w $h $fps $rc 2000 150 $ri 2>&1); code=$?
    if [ $code -ne 0 ]; then
      echo "FAIL ${w}x${h}@${fps} $rc reinit=$ri exit=$code :: $(echo "$out" | tail -1)"
      fails=$((fails+1))
    fi
   done
  done
 done
done
echo "matrix done, failures=$fails"
