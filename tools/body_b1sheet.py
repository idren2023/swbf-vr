"""python -I tools/body_b1sheet.py <out.png> <dump.json> ..: see-through sheets of the B1's grips (tools/body_b1grab.py
dump=): per grip a row of the weapon's triangles (faint) with the hand's, each digit its colour, from the thumb side
(the hand's -x), from the back of the hand (+z) and from the fingertips (+y); the trigger a red cross, the controller's
grip axis a dashed line. Arm space in mm."""
import json, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection

COLOURS = {'hand': '#888888', 'forearm': '#666666', 'trigger_1': '#1f5fd0', 'trigger_2': '#6fb0ff',
           'grip_1': '#1a9a3a', 'grip_2': '#7ddc6a', 'thumb_1': '#d05a10', 'thumb_2': '#ffaa33'}
VIEWS = (('thumb side (y right, z up)', 1, 2, 0, 1), ('back of the hand (x right, y up)', 0, 1, 2, -1),
         ('fingertips (x right, z up)', 0, 2, 1, 1))
dumps = [json.load(open(p)) for p in sys.argv[2:]]
fig, axes = plt.subplots(len(dumps), 3, figsize=(18, 6 * len(dumps)), squeeze=False)
for row, d in zip(axes, dumps):
    H, W = np.array(d['hand']), np.array(d['weapon'])
    for a, (title, i, j, depth, sign) in zip(row, VIEWS):
        # the weapon behind, faint; the hand's triangles drawn far to near (on top where nearer the viewer)
        a.add_collection(PolyCollection([W[t][:, [i, j]] for t in d['weapon_tris']], facecolors='#bbbbbb',
                                        edgecolors='#777777', linewidths=0.2, alpha=0.25))
        tris = sorted(d['hand_tris'], key=lambda t: sign * H[t, depth].mean())
        a.add_collection(PolyCollection([H[t][:, [i, j]] for t in tris],
                                        facecolors=[COLOURS.get(d['bone'][t[0]], '#888') for t in tris],
                                        edgecolors='none', alpha=0.9))
        if d['trigger']:
            a.plot(d['trigger'][i], d['trigger'][j], 'rx', ms=12, mew=2.5)
        p, ax_ = np.array(d['axis_point']), np.array(d['axis'])
        line = np.array([p - ax_ * 60, p + ax_ * 60])
        a.plot(line[:, i], line[:, j], 'k--', lw=1)
        a.set_xlim(-90, 90) if i == 0 else a.set_xlim(-40, 140)
        a.set_ylim(-110, 40) if j == 2 else a.set_ylim(-40, 140)
        a.set_aspect('equal')
        a.grid(alpha=0.3)
        a.set_title('%s: %s' % (d['grip'], title), fontsize=10)
plt.tight_layout()
plt.savefig(sys.argv[1], dpi=70)
print(sys.argv[1])
