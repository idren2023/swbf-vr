"""uv transfer from a stock mesh onto a retopology mesh lying on its surface (tools/body_transfer.py, tools/body_forearm.py):
the stock's uv layout is used unchanged. Each corner of a retopo face takes the uv of the nearest point on the stock,
unless that point lies across a seam from the stock face under the retopo face's centre: then the nearest point on the
centre's side (the seam's edge), so no face spans a seam; faces on either side of a seam get split uvs at their shared
vertices."""
import bmesh
from mathutils import geometry
from mathutils.bvhtree import BVHTree


def tri_bm(mesh):
    b = bmesh.new()
    b.from_mesh(mesh)
    bmesh.ops.triangulate(b, faces=b.faces[:])
    b.verts.ensure_lookup_table()
    b.faces.ensure_lookup_table()
    return b


def transfer_uv(me, stock_mesh):
    """Returns (stock seam edges, corners clamped to a seam)."""
    sb = tri_bm(stock_mesh)
    uvl = sb.loops.layers.uv.active

    # a seam edge has different uvs on its two sides: faces are joined across an edge only where both its ends have
    # the same uvs on either side
    def joined(g, h, e):
        for v in e.verts:
            ua = next(x[uvl].uv for x in g.loops if x.vert is v)
            ub = next(x[uvl].uv for x in h.loops if x.vert is v)
            if (ua - ub).length > 1e-5:
                return False
        return True

    nbr = {f: [l2.face for l in f.loops for l2 in l.edge.link_loops if l2.face is not f and joined(f, l2.face, l.edge)]
           for f in sb.faces}
    seams = sum(1 for e in sb.edges if len(e.link_faces) == 2 and not joined(e.link_faces[0], e.link_faces[1], e))
    whole = BVHTree.FromBMesh(sb)

    def patch(f, depth=3):
        """f and the faces reachable from it within depth steps without crossing a seam."""
        out, front = {f}, [f]
        for _ in range(depth):
            front = [h for g in front for h in nbr[g] if h not in out]
            out.update(front)
        return out

    def uv_at(p, f):
        a, b, c = (l.vert.co for l in f.loops)
        ua, ub, uc = (l[uvl].uv for l in f.loops)
        return geometry.barycentric_transform(p, a, b, c, ua.to_3d(), ub.to_3d(), uc.to_3d()).to_2d()

    if not me.uv_layers:
        me.uv_layers.new(name='UV')
    uv = me.uv_layers.active.data
    clamped = 0
    for poly in me.polygons:
        _, _, fi, _ = whole.find_nearest(poly.center)
        near_ = patch(sb.faces[fi])
        for li in poly.loop_indices:
            p = me.vertices[me.loops[li].vertex_index].co
            loc, _, j, _ = whole.find_nearest(p)
            f = sb.faces[j]
            if f not in near_:
                # past a seam from the face's centre: the nearest point of the centre's side (the seam's edge)
                clamped += 1
                best = None
                for g in near_:
                    q = geometry.closest_point_on_tri(p, *(x.co for x in g.verts))
                    if best is None or (q - p).length < best[0]:
                        best = ((q - p).length, q, g)
                loc, f = best[1], best[2]
            uv[li].uv = uv_at(loc, f)
    return seams, clamped
