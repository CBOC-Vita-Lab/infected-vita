#!/usr/bin/env python3
"""pools.py: print the allocator pool table the game builds at 0x08892BF0 (name id elem count align)."""
import re,struct,sys
img=open('/home/claude/infected/work/image.bin','rb').read(); base=0x08804000
def cstr(a):
    o=a-base; e=img.index(b'\0',o); return img[o:e].decode('latin1')
start=int(sys.argv[1],16) if len(sys.argv)>1 else 0x08892bf0
end=int(sys.argv[2],16) if len(sys.argv)>2 else 0x08893000
name=None; ids=None; idv=None; elem=None; cnt=None; lui5=None
rx=re.compile(r'^([0-9a-f]{8}): (\S+)\s*(.*)$')
cur=[]
for l in open('/home/claude/ws/tune/private/dis/image.txt'):
    m=rx.match(l)
    if not m: continue
    a=int(m.group(1),16)
    if a<start: continue
    if a>=end: break
    op,args=m.group(2),m.group(3).replace('\t',' ')
    if op=='lui' and args.startswith('$5,'): lui5=int(args.split(',')[1],16)<<16
    elif op=='addiu' and args.startswith('$5, $5,') and lui5 is not None:
        v=(lui5+int(args.split(',')[2],0))&0xffffffff
        try: name=cstr(v)
        except Exception: name='?%x'%v
    elif op=='ori' and args.startswith('$5, $zero,'): elem=int(args.split(',')[2],0)
    if op=='jal' and args.strip()=='0x8896e28': idv=cnt
    if op=='jal' and args.strip()=='0x889700c':
        print('%08x id=%-2s name=%-24s elem=%#x (%d) count=%s'%(a,idv,name,elem,elem,cnt))
    if op=='ori' and args.startswith('$6, $zero,'): cnt=int(args.split(',')[2],0)
