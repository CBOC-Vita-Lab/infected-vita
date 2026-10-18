#!/usr/bin/env python3
"""bspext.py STREAM..: list the world BSP resources of level streams with their bounding boxes (RpWorld chunk info)."""
import struct,sys,re,glob,os
def chunks(d):
    pos=0
    while pos+12<=len(d):
        t,sz,st=struct.unpack_from('<III',d,pos); yield pos,t,sz; pos+=12+sz
for f in sys.argv[1:]:
    d=open(f,'rb').read()
    for p,t,sz in chunks(d):
        if t!=0x716: continue
        body=d[p+12:p+12+sz]
        m=re.match(rb'.{8}([\w\.]+\.bsp)\0',body[:80],re.S)
        if not m or b'rwID_WORLD' not in body[:120]: continue
        name=m.group(1).decode()
        # find world chunk: 0x0B + size + version, followed by struct chunk 0x01
        for i in range(0,min(len(body)-40,600)):
            t0,s0,v0=struct.unpack_from('<III',body,i)
            if t0==0x0B and s0<=len(body) and (v0>>16) in (0x1c02,0x1803,0x1802,0x1c03):
                t1,s1,v1=struct.unpack_from('<III',body,i+12)
                if t1==1 and s1 in (64,76):
                    raw=body[i+24:i+24+s1]
                    tri,vert,ps,ws,col=struct.unpack('<5I',raw[16:36])
                    bb=struct.unpack('<6f',raw[40:64])
                    sup=bb[:3]; inf=bb[3:]
                    ext=[abs(sup[k]-inf[k]) for k in range(3)]
                    print('%-16s %-28s size=%7d tri=%6d vtx=%6d sectors=%3d inf=(%.0f %.0f %.0f) sup=(%.0f %.0f %.0f) ext=(%.0f %.0f %.0f)'%(os.path.basename(f)[:-7],name,sz,tri,vert,ws,*inf,*sup,*ext))
                    break
