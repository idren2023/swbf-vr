#!/bin/sh
# Every model of ours from git plus the installed game (idren2023 2026-11-04), into re/body/, then the two includes the
# build embeds:
#   sh tools/body_rebuild_all.sh [force]
# 1. tools/body_textures.sh: our textures from the game's (Upscayl for the 4x ones; without it, unscaled);
# 2. tools/body_rebuild.py: re/body/r_arm_grips.blend (the arms: tools/data/models/arm) and re/body/player_stock.blend
#    (the player body: tools/data/models/player); an existing blend is kept unless `force` (it may hold unsaved work:
#    save it first with tools/body_save.py);
# 3. tools/body_export.py -> re/body/BodyArm.inc, tools/body_export_player.py -> re/body/PlayerBody.inc.
# The player body's baked textures come from tools/body_bake.py (recipe tools/data/models/player/bake.json).
cd "$(dirname "$0")/.." || exit 1
B="${BLENDER:-C:/Program Files/Blender Foundation/Blender 4.5/blender.exe}"
sh tools/body_textures.sh || exit 1
force=$1
for k in "arm r_arm_grips" "player player_stock"; do
  set -- $k
  if [ -f "re/body/$2.blend" ] && [ "$force" != force ]; then
    echo "body_rebuild_all: re/body/$2.blend kept (pass force to rebuild it)"
  else
    "$B" -b --factory-startup -P tools/body_rebuild.py -- "re/body/$2.blend" set=$1 2>&1 | grep -E "REBUILD|Error" || exit 1
  fi
done
if [ -f tools/data/models/player/bake.json ] && { [ "$force" = force ] || [ ! -f re/body/player_arms.png ]; }; then
  "$B" -b --factory-startup -P tools/body_bake.py 2>&1 | grep -E "BAKE|Error"   # the player body's baked textures
fi
"$B" -b --factory-startup re/body/r_arm_grips.blend -P tools/body_export.py -- re/body/BodyArm.inc 2>&1 | grep -E "EXPORT skel|Error"
if [ -f re/body/player_arms.png ] && [ -f re/body/player_hands.png ]; then
  "$B" -b --factory-startup re/body/player_stock.blend -P tools/body_export_player.py -- re/body/PlayerBody.inc arm=re/body/BodyArm.inc 2>&1 | grep -E "EXPORT ->|Error"
else
  echo "body_rebuild_all: re/body/player_arms.png / player_hands.png missing (PLAN: the player body's baked textures): PlayerBody.inc not written"
fi
