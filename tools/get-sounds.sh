#!/bin/bash
# Pulls the thunder and ambient sounds the plugin embeds out of YOUR OWN copy of
# Diablo II: Resurrected and converts them to WAV. The game's sound files are
# Blizzard's and are not part of this repository.
#
# Linux: needs wine (with winegcc), ffmpeg, and CascLib.dll (D2RMM ships one in its
# tools folder).
#   ./get-sounds.sh "/path/to/Diablo II Resurrected" "/path/to/D2RMM/tools/CascLib.dll"
set -e
GAME="$1"; CASCLIB="$2"
[ -d "$GAME" ] && [ -f "$CASCLIB" ] || { echo "usage: $0 <game folder> <CascLib.dll>"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"; export WINEPREFIX="$WORK/prefix" WINEDEBUG=-all
cp "$CASCLIB" "$WORK/CascLib.dll"
( cd "$WORK" && winegcc -m64 -o cx "$HERE/casc_extract/cx.c" )
WINGAME="Z:$(echo "$GAME" | sed 's|/|\\|g')"; WINOUT="Z:$(echo "$WORK/out" | sed 's|/|\\|g')"; mkdir -p "$WORK/out"
{
  for n in 1 2 3 4; do echo "data:data\\hd\\global\\sfx\\ambient\\event\\thunder_norm_${n}_hd.flac"; done
  for n in breath1 breath3 breath6 breath8 moan1 moan2 moan3 moan4 moan5 moan6 moan7 moan8 scream1 scream3 scream5 scream7 raven1 raven3; do
    echo "data:data\\global\\sfx\\ambient\\event\\${n}.flac"; done
  echo "data:data\\global\\sfx\\monster\\diablo\\laugh1.flac"
} > "$WORK/names.txt"
( cd "$WORK" && wine ./cx.exe.so CascLib.dll "$WINGAME" get "$WINOUT" < names.txt )
RES="$HERE/../plugin/res"; mkdir -p "$RES/amb"
i=1
for n in 1 2 3 4; do
  ffmpeg -v error -y -i "$WORK/out/data/hd/global/sfx/ambient/event/thunder_norm_${n}_hd.flac" -ac 1 -ar 32000 \
    -af "bass=g=4:f=90,volume=12dB,alimiter=limit=0.95:level=false:attack=3:release=120" -c:a pcm_s16le -bitexact "$RES/thunder$n.wav"
done
for n in breath1 breath3 breath6 breath8 moan1 moan2 moan3 moan4 moan5 moan6 moan7 moan8 scream1 scream3 scream5 scream7 raven1 raven3; do
  ffmpeg -v error -y -i "$WORK/out/data/global/sfx/ambient/event/$n.flac" -ac 1 -ar 22050 -t 9 \
    -af "volume=3dB,alimiter=limit=0.95:level=false" -c:a pcm_s16le -bitexact "$RES/amb/a$i.wav"; i=$((i+1))
done
ffmpeg -v error -y -i "$WORK/out/data/global/sfx/monster/diablo/laugh1.flac" -ac 1 -ar 22050 -t 9 \
  -af "volume=3dB,alimiter=limit=0.95:level=false" -c:a pcm_s16le -bitexact "$RES/amb/a$i.wav"
rm -rf "$WORK"
echo "Sounds written to $RES"
