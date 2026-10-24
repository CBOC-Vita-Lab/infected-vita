#!/usr/bin/env python3
# Slice one guest function (start,end) out of a real generated unit, wrap it in a minimal standalone function
# with the same prologue/LOCAL_DISPATCH scaffold the generator emits.  Output: current-style C++ for the function.
import sys,re
start=int(sys.argv[1],16); end=int(sys.argv[2],16); name=sys.argv[3]
base=0x08804000; unit=(start-base)//0x4000
path='/work/assess/experiments/gen/generated_unit_%04d.cpp'%unit
lines=open(path).read().split('\n')
def find(addr):
    pat='L_%08X:'%addr
    for i,l in enumerate(lines):
        if l==pat: return i
    raise SystemExit('label %s not found'%pat)
a=find(start)
try: b=find(end)
except SystemExit: b=None
if b is None: raise
body=lines[a:b]
labels=[int(re.match(r'L_([0-9A-F]{8}):',l).group(1),16) for l in body if re.match(r'L_[0-9A-F]{8}:',l)]
out=[]
out.append('#include "psprecomp/corpus_abi.hpp"')
out.append('#include "recomp_units.hpp"')
out.append('#include <atomic>\n#include <bit>\n#include <cmath>\n#include <cstdint>\n#include <limits>')
out.append('namespace psprecomp {')
out.append('extern const std::uint16_t kEntryIds_slice[4096];')
out.append('void %s(CorpusRuntime &rt, AllegrexContext &ctx, std::uint16_t direct_entry_id, AotFastView &aot_mem) {'%name)
out.append('    std::uint32_t jump_target = 0u;\n    std::uint32_t local_transfers = 0u;\n    std::uint32_t local_pc = ctx.pc;\n    std::uint32_t entry_id = direct_entry_id;')
out.append('LOCAL_DISPATCH:\n    {\n    if (entry_id == 0u) {\n        const std::uint32_t entry_delta = local_pc - 0x%08Xu;\n        entry_id = (entry_delta < 16384u && (entry_delta & 3u) == 0u) ? kEntryIds_slice[entry_delta >> 2u] : 0u;\n    }\n    switch (entry_id) {'%(base+unit*0x4000))
for i,l in enumerate(labels): out.append('    case %du: goto L_%08X;'%(i+1,l))
out.append('    default: ctx.pc = local_pc; return;\n    }\n    }')
out+=body
out.append('L_%08X:\n    ctx.pc = 0x%08Xu; return;'%(end,end))
out.append('}\n}')
open(sys.argv[4],'w').write('\n'.join(out)+'\n')
print(name,'labels',len(labels),'lines',len(body))
