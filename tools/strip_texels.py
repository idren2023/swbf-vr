"""python tools/strip_texels.py <in.inc> <out.inc>: the arm or body include with its texture pixels left out (each
array cut to one zero; GameTextures makes the pixels at start from the player's own game textures and our differences,
tools/texpatch.py). The rest is copied as it is."""
import re
import sys

text = open(sys.argv[1]).read()
text, count = re.subn(r'constexpr unsigned ((?:BODY_ARM_TEXTURE|PLAYER_BODY_TEXELS)_\d+)\[\] = \{.*?\};',
                      r'constexpr unsigned \1[1] = {};  // made at start: GameTextures', text, flags=re.S)
open(sys.argv[2], 'w').write(text)
print('strip_texels: %d texture arrays left out of %s' % (count, sys.argv[2]))
