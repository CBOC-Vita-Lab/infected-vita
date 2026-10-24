#!/bin/bash
# d.sh START [COUNT]  : print COUNT (default 64) relocated instructions starting at guest address START
python3 - "$@" <<'PY'
import sys,os,pickle,bisect
a=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 64
fn='/work/ws/tune/private/dis/image.txt'
idxf='/work/ws/tune/private/dis/image.idx'
if not os.path.exists(idxf):
    addrs=[];offs=[];p=0
    with open(fn,'rb') as f:
        for l in f:
            addrs.append(int(l[:8],16));offs.append(p);p+=len(l)
    pickle.dump((addrs,offs),open(idxf,'wb'))
addrs,offs=pickle.load(open(idxf,'rb'))
i=bisect.bisect_left(addrs,a)
f=open(fn,'rb'); f.seek(offs[i])
for _ in range(n):
    l=f.readline()
    if not l: break
    sys.stdout.write(l.decode())
PY
