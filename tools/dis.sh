#!/bin/bash
# usage: dis.sh START_ADDR [END_ADDR]  (guest addresses, base 0x08804000)
BASE=$((0x08804000))
S=$(( $1 - BASE ))
E=$(( ${2:-$(( $1 + 0x80 ))} - BASE ))
llvm-objdump -d --triple=mipsel --no-show-raw-insn /work/infected/work/image.o --start-address=$S --stop-address=$E 2>&1 | python3 -c '
import sys,re
base=0x08804000
for l in sys.stdin:
    m=re.match(r"\s*([0-9a-f]+):\s+(.*)",l)
    if m:
        a=int(m.group(1),16)+base
        t=re.sub(r"<[^>]*>","",m.group(2)).rstrip()
        mm=re.match(r"(b\w*)\s+(.*?)(0x[0-9a-f]+)$",t)
        if mm and not t.startswith(("break","bitrev")):
            t="%s %s0x%x"%(mm.group(1),mm.group(2),int(mm.group(3),16)+base)
        print("%08x: %s"%(a,t))
'
