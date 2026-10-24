import struct, sys, collections
BASE=0x08804000
img=open('/work/infected/work/image.bin','rb').read()
N=0x2860D4//4  # code words
words=struct.unpack('<%dI'%N, img[:N*4])
seeds=set()
for i,w in enumerate(words):
    if (w>>26)==3: seeds.add(((w&0x3ffffff)<<2)|0x08000000 if False else (w&0x3ffffff)<<2)
seeds={s for s in seeds if BASE<=s<BASE+N*4}
# also function starts after 'jr ra' + delay slot followed by addiu sp,sp,-X
for i in range(1,N-2):
    w=words[i]
    if (w>>26)==9 and ((w>>16)&31)==29 and ((w>>21)&31)==29 and (w&0x8000):
        # prologue: only treat as seed if previous-previous is jr ra
        if (words[i-2]==0x03e00008) or (words[i-1]==0x03e00008):
            seeds.add(BASE+i*4)
seeds=sorted(seeds)
print('seeds',len(seeds))
funcs=[]
for k,s in enumerate(seeds):
    e=seeds[k+1] if k+1<len(seeds) else BASE+N*4
    funcs.append((s,e))
def stats(s,e):
    c=collections.Counter()
    i0=(s-BASE)//4; i1=(e-BASE)//4
    nops=0; back=0
    for i in range(i0,i1):
        w=words[i]; op=w>>26
        if w==0: nops+=1
        if op==0x23 or op in (0x20,0x24,0x21,0x25): c['ld']+=1
        elif op in (0x2b,0x28,0x29): c['st']+=1
        elif op in (0x31,): c['lwc1']+=1
        elif op in (0x39,): c['swc1']+=1
        elif op==0x11:
            c['cop1']+=1
        elif op==3: c['jal']+=1
        elif op==0 and (w&0x3f)==9: c['jalr']+=1
        elif op==0 and (w&0x3f)==8 and ((w>>21)&31)!=31: c['jr_other']+=1
        elif op in (4,5,6,7,0x14,0x15,0x16,0x17) or (op==1 and ((w>>16)&31) in (0,1,2,3,16,17)):
            off=struct.unpack('<h',struct.pack('<H',w&0xffff))[0]
            c['br']+=1
            if off<0: back+=1
    c['back']=back; c['n']=i1-i0; c['nop']=nops
    return c
res=[(s,e,stats(s,e)) for s,e in funcs]
def show(tag,lst):
    print('==',tag)
    for s,e,c in lst[:6]:
        print('  %08x-%08x n=%d ld=%d st=%d lwc1=%d swc1=%d cop1=%d jal=%d jalr=%d br=%d back=%d'%(s,e,c['n'],c['ld'],c['st'],c['lwc1'],c['swc1'],c['cop1'],c['jal'],c['jalr'],c['br'],c['back']))
leaf=[r for r in res if 24<=r[2]['n']<=40 and r[2]['jal']==0 and r[2]['jalr']==0 and r[2]['back']==0 and r[2]['cop1']==0 and r[2]['ld']+r[2]['st']>=10]
loop=[r for r in res if 20<=r[2]['n']<=50 and r[2]['jal']==0 and r[2]['jalr']==0 and r[2]['back']>=1 and r[2]['cop1']==0]
call=[r for r in res if 40<=r[2]['n']<=80 and r[2]['jal']>=3 and r[2]['cop1']==0]
flt=[r for r in res if 30<=r[2]['n']<=100 and (r[2]['cop1']+r[2]['lwc1']+r[2]['swc1'])>=0.4*r[2]['n'] and r[2]['jal']==0]
show('leaf',leaf); show('loop',loop); show('call',call); show('float',flt)
print('len hist: ', end='')
import statistics
ns=[r[2]['n'] for r in res]
print('median',statistics.median(ns),'mean',sum(ns)/len(ns),'max',max(ns))
print('funcs with jr_other',sum(1 for r in res if r[2]['jr_other']))
print('funcs with back edge',sum(1 for r in res if r[2]['back']),' with jal',sum(1 for r in res if r[2]['jal']),' leaf (no jal/jalr)',sum(1 for r in res if r[2]['jal']==0 and r[2]['jalr']==0))
