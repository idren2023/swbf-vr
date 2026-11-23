"""Blender (blender -b --factory-startup <file.blend> -P tools/body_fix_textured.py): every material's image node made
its active node, then the blend saved (idren2023 2026-11-04: textured views of everything). Workbench's TEXTURE colour
mode draws a material's active image node; materials whose active node was another one (the output: msh imports,
older tools) rendered magenta in tools/body_poses.py and the other sheet tools."""
import bpy

fixed = 0
for m in bpy.data.materials:
    if not m.use_nodes:
        continue
    tex = [n for n in m.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image]
    if tex and m.node_tree.nodes.active not in tex:
        m.node_tree.nodes.active = tex[0]
        fixed += 1
if fixed:
    bpy.ops.wm.save_mainfile(compress=True)
print('FIXTEXTURED %s: %d materials fixed' % (bpy.path.basename(bpy.data.filepath), fixed))
