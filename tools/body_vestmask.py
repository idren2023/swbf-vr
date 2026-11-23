import bpy, numpy as np, os
S=1932.0
POLYS={  # displayed-pixel points of idren2023's outlines (1932 px image), in order round each region
'pad':[(95,700),(130,640),(380,632),(650,632),(705,680),(690,760),(620,850),(520,950),(300,950),(160,890),(100,800)],
'back':[(670,575),(745,570),(752,525),(900,528),(955,645),(1110,645),(1160,540),(1270,530),(1330,415),(1450,410),(1460,520),(1560,640),(1565,950),(1505,1110),(1350,1120),(860,1100),(690,1055),(668,950),(690,800),(745,640)],
'front':[(1750,465),(1915,470),(1910,1095),(1790,1060),(1680,960),(1630,900),(1622,760),(1645,640),(1690,560)],
}
im=bpy.data.images.load(os.path.abspath('re/body/hd/All_Inf_trooper-hd.png')); W,H=im.size
px=np.array(im.pixels[:],dtype=np.float32).reshape(H,W,4)
ys,xs=np.mgrid[0:H,0:W]; u=(xs+0.5)/W; v=(ys+0.5)/H
def inside(poly):
    pts=[(x/S,1-y/S) for x,y in poly]; res=np.zeros((H,W),bool)
    for (x1,y1),(x2,y2) in zip(pts,pts[1:]+pts[:1]):
        cond=((y1>v)!=(y2>v)); xint=(x2-x1)*(v-y1)/((y2-y1) if y2!=y1 else 1e-9)+x1
        res^=cond&(u<xint)
    return res
region=np.zeros((H,W),bool)
for n,p in POLYS.items(): region|=inside(p)
r,g,b=px[...,0],px[...,1],px[...,2]; lum=0.3*r+0.59*g+0.11*b; sat=np.max(px[...,:3],-1)-np.min(px[...,:3],-1)
# the vest's beige: red at least green (the lapels' dark olive has about as much of each: idren2023, 2026-10-25, it was
# taken in), clearly warmer than blue (not the grey shirt), and not dark (not the green band under the hem)
# by hue, not brightness (the vest's own shading is as dark as the green): beige's green sits well below its red
# (above blue), the olive green's about level with or above it; grey has no hue; near-black is out
chroma=r-b
beige=(chroma>0.035)&((g-b)<0.86*chroma)&(lum>0.14)
mask=region&beige
def grow(m,r):
    out=m.copy()
    for dy in range(-r,r+1):
        for dx in range(-r,r+1):
            if dx*dx+dy*dy<=r*r: out|=np.roll(np.roll(m,dy,0),dx,1)
    return out
def shrink(m,r): return ~grow(~m,r)
mask=grow(shrink(mask,3),3)              # specks dropped
# holes inside the vest (its own dark folds and pocket shadows) filled: what of the non-vest a spread from outside the
# regions cannot reach is enclosed by vest; the lapels' and the hem's green run out to the regions' edges and stay out
reach=~region
for _ in range(400):
    nxt=reach.copy()
    for dy,dx in ((1,0),(-1,0),(0,1),(0,-1)):
        nxt|=np.roll(np.roll(reach,dy,0),dx,1)
    nxt&=~mask
    if (nxt==reach).all(): break
    reach=nxt
mask=~reach
mask&=region
out=np.zeros((H,W,4),np.float32); out[...,:3]=mask[...,None]; out[...,3]=1
mi=bpy.data.images.new('vestmask',W,H); mi.pixels[:]=out.ravel(); mi.filepath_raw=os.path.abspath('re/body/vest_mask.png'); mi.file_format='PNG'; mi.save()
# overlay preview: the texture dimmed outside the mask, the mask's edge in red
prev=px.copy(); prev[...,:3]=np.where(mask[...,None],prev[...,:3],prev[...,:3]*0.35)
# full-resolution crops of the three regions to look at closely
for nm,(x0,y0,x1,y1) in {'pad':(0.03,0.48,0.37,0.70),'back':(0.34,0.40,0.82,0.80),'front':(0.80,0.40,1.0,0.80)}.items():
    X0,X1,Y0,Y1=int(x0*W),int(x1*W),int(y0*H),int(y1*H)
    c=px[Y0:Y1,X0:X1].copy(); m=mask[Y0:Y1,X0:X1]
    c[...,:3]=np.where(m[...,None],c[...,:3],c[...,:3]*0.25)
    e=m^np.roll(m,1,0)|m^np.roll(m,1,1); c[e,:3]=(1,0,0)
    ci=bpy.data.images.new('crop_'+nm,X1-X0,Y1-Y0); ci.pixels[:]=c.ravel(); ci.filepath_raw=os.path.abspath('re/body/vest_mask_%s.png'%nm); ci.file_format='PNG'; ci.save()
edge=mask^np.roll(mask,1,0)|mask^np.roll(mask,1,1); prev[edge,:3]=(1,0,0)
pi=bpy.data.images.new('prev',W,H); pi.pixels[:]=prev.ravel(); pi.scale(1200,1200); pi.filepath_raw=os.path.abspath('re/body/vest_mask_preview.png'); pi.file_format='PNG'; pi.save()
print('MASK region %.1f%%, vest %.1f%% of the texture'%(region.mean()*100, mask.mean()*100))
