import struct,sys
def nodes(p):
    d=open(p,'rb').read(); out=[]; i=0
    while True:
        i=d.find(b'MODL',i)
        if i<0: break
        n=struct.unpack_from('<I',d,i+4)[0]; b=d[i+8:i+8+n]; r={}
        j=0
        while j+8<=len(b):
            t=b[j:j+4]; l=struct.unpack_from('<I',b,j+4)[0]; c=b[j+8:j+8+l]
            if t==b'NAME': r['name']=c.split(b'\0')[0].decode('latin1')
            elif t==b'PRNT': r['parent']=c.split(b'\0')[0].decode('latin1')
            elif t==b'TRAN': r['tran']=struct.unpack_from('<10f',c)
            elif t==b'MTYP': r['type']=struct.unpack_from('<I',c)[0]
            j+=8+l+((-l)%4)
        out.append(r); i+=8
    return out
if __name__=='__main__':
    for p in sys.argv[1:]:
        print('##',p.split('/')[-1])
        for r in nodes(p):
            t=r.get('tran',(0,)*10)
            print('  %-26s parent %-18s type %s  rot %.3f %.3f %.3f %.3f  pos %.3f %.3f %.3f'%(r.get('name'),r.get('parent','-'),r.get('type'),t[3],t[4],t[5],t[6],t[7],t[8],t[9]))
