#!/bin/sh
# Our arm and body textures, made again from the game's own (idren2023 2026-11-04: every model we made rebuildable from
# git plus the installed game; the results are the game's pixels reworked, so they stay in re/, never in git).
#   sh tools/body_textures.sh [check]
# check: also compares each result with the file already there (byte for byte by pixel) and reports.
# Needs: the mod tools' assets (C:/LucasArts/BFBuilder/Assets), Blender 4.5, re/pyenv (Pillow, numpy), and Upscayl 2.15
# (https://upscayl.org; its command line tool and models). Without Upscayl the upscaled textures fall back to the game
# texture at its own size (a warning; the arm then looks softer).
cd "$(dirname "$0")/.." || exit 1
A=C:/LucasArts/BFBuilder/Assets/Sides
B="${BLENDER:-C:/Program Files/Blender Foundation/Blender 4.5/blender.exe}"
U="${UPSCAYL:-C:/Program Files/Upscayl/resources}"
PY="${PYTHON:-re/pyenv/Scripts/python.exe}"
H=re/body/hd
T=re/body/textures-new
mkdir -p "$H" "$T"
CHECK=$1

png() { # <tga> <png>: the game texture as png, as it is
  "$PY" -I -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$1" "$2" || exit 1
}
upscale() { # <png> <model> <out>: 4x by Upscayl's model, or the input as it is without Upscayl
  if [ -x "$U/bin/upscayl-bin.exe" ]; then
    "$U/bin/upscayl-bin.exe" -i "$1" -o "$3" -s 4 -m "$U/models" -n "$2" -f png >/dev/null 2>&1 || exit 1
  else
    echo "body_textures: Upscayl not found at $U (set UPSCAYL): $3 is the game texture unscaled" >&2
    cp "$1" "$3"
  fi
}
halve() { # <png> <out> <side>: scaled in Blender (Image.scale), as the 512 px copies were made
  "$B" -b --factory-startup --python-expr "import bpy, sys; im = bpy.data.images.load(sys.argv[-3]); s = int(sys.argv[-1]); im.scale(s, s); im.filepath_raw = sys.argv[-2]; im.file_format = 'PNG'; im.save()" -- "$1" "$2" "$3" >/dev/null 2>&1 || exit 1
}
put() { # <made> <target>: into place (check: compared with the old one first)
  if [ "$CHECK" = check ] && [ -f "$2" ]; then
    "$PY" -I -c "
import sys, numpy as np
from PIL import Image
a = np.array(Image.open(sys.argv[1]).convert('RGBA')).astype(int); b = np.array(Image.open(sys.argv[2]).convert('RGBA')).astype(int)
print('  %-55s %s' % (sys.argv[2], 'same' if a.shape == b.shape and (a == b).all() else
      ('DIFFERS: max %d mean %.3f' % (abs(a - b).max(), abs(a - b).mean()) if a.shape == b.shape else 'DIFFERS: size %s %s' % (a.shape, b.shape))))" "$1" "$2"
  fi
  mv "$1" "$2"
}

# the game textures as png (Upscayl's input)
png $A/ALL/MSH/All_Inf_trooper.tga $T/All_Inf_trooper.png
png $A/ALL/MSH/All_Inf_Trooper_packs.tga $T/All_Inf_Trooper_packs.png
png $A/ALL/MSH/all_1st_inf_trooper_arms.tga $T/all_1st_inf_trooper_arms.png
png $A/ALL/MSH/all_1st_inf_trooper_hands.tga $T/all_1st_inf_trooper_hands.png
png $A/CIS/MSH/cis_1st_sbdroid_hands.tga $T/cis_1st_sbdroid_hands.png
# the player body's textures 4x (Upscayl high-fidelity-4x: tools/body_assemble.py, body_garments.py)
upscale $T/All_Inf_trooper.png high-fidelity-4x $T/a.png && put $T/a.png $H/All_Inf_trooper-hd.png
cp $H/All_Inf_trooper-hd.png $T/a.png && put $T/a.png $H/All_Inf_trooper-high-fidelity-4x.png  # (the same; player_stock's name)
upscale $T/All_Inf_Trooper_packs.png high-fidelity-4x $T/a.png && put $T/a.png $H/All_Inf_Trooper_packs-hd.png
# the first-person glove and sleeve 4x, then halved to 512 (the player body's arms)
for n in all_1st_inf_trooper_arms all_1st_inf_trooper_hands; do
  upscale $T/$n.png high-fidelity-4x $T/a.png && put $T/a.png $H/$n-4x.png
  halve "$(pwd -W)/$H/$n-4x.png" "$(pwd -W)/$T/a.png" 512 && put $T/a.png $H/$n-hd.png
done
# the super battle droid's first-person texture 4x (Upscayl ultrasharp-4x: tools/body_sbd.py, BodyArm)
upscale $T/cis_1st_sbdroid_hands.png ultrasharp-4x $T/a.png && put $T/a.png $H/cis_1st_sbdroid_hands-ultrasharp-4x.png
# the Rebel sleeve in the body's grey (tools/body_recolour.py): the game's size for BodyArm, the 512 and 4x copies
for k in "$A/ALL/MSH/all_1st_inf_trooper_arms.tga re/body/all_1st_inf_trooper_arms_grey.png" \
         "$H/all_1st_inf_trooper_arms-hd.png $H/all_1st_inf_trooper_arms_grey-hd.png" \
         "$H/all_1st_inf_trooper_arms-4x.png $H/all_1st_inf_trooper_arms_grey-4x.png"; do
  set -- $k
  "$B" -b --factory-startup -P tools/body_recolour.py -- "$1" "$(pwd -W)/$T/a.png" >/dev/null 2>&1 || exit 1
  put $T/a.png "$2"
done
# the Wookiee: its hands baked into our glove's layout (tools/body_unithands.py), its texture with alpha for the hair
"$PY" -I tools/body_unithands.py $A/ALL/MSH/all_1st_wookie.msh $T/a.png >/dev/null || exit 1
put $T/a.png re/body/all_1st_inf_wookie_hands.png
png $A/ALL/MSH/all_1st_inf_wookie.tga $T/a.png && put $T/a.png re/body/all_1st_inf_wookie_hair.png
rm -rf $T
echo "body_textures: done"
