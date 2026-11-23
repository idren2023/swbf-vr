"""python tools/body_setpose.py "<pose label>" "<TUNEPOSE line or dict>" [comment]: writes a tuned pose (the
TUNEPOSE line tools/body_tunegrip.py prints) into tools/body_poses.py, replacing that label's entry."""
import ast, re, sys

label, text = sys.argv[1], sys.argv[2]
comment = sys.argv[3] if len(sys.argv) > 3 else 'tuned (tools/body_tunegrip.py)'
pose = ast.literal_eval(text[text.index('{'):])
p = 'tools/body_poses.py'
s = open(p).read()
pattern = re.compile(r"(?:        # [^\n]*\n)*        \('%s', pose\([^\n]*\)\),\n" % re.escape(label))
m = pattern.search(s)
assert m, label
entry = "        # %s\n        ('%s', pose(%s)),\n" % (comment, label, ', '.join('%s=%s' % kv for kv in pose.items()))
s = s[:m.start()] + entry + s[m.end():]
open(p, 'w').write(s)
print('set', label)
