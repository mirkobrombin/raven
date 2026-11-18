# Compares an oracle run on x86 silicon with the Rosetta run of the same cases, field by field as the tests compare them.
#   python3 -I compare_oracles.py int|sse ROSETTA.bin X86.bin
import sys
kind = sys.argv.pop(1)
if kind == 'sse':
    import struct,sys,collections
    IO=656
    def read(p):
        d=open(p,'rb').read();o=0
        n,=struct.unpack_from('<I',d,o);o+=4;out=[]
        for _ in range(n):
            l,=struct.unpack_from('<I',d,o);o+=4;code=d[o:o+l];o+=l
            l,=struct.unpack_from('<I',d,o);o+=4;text=d[o:o+l].decode();o+=l
            l,=struct.unpack_from('<I',d,o);o+=4;fam=d[o:o+l].decode();o+=l
            inp=d[o:o+IO];o+=IO;outp=d[o:o+IO];o+=IO
            out.append((text,fam,inp,outp))
        return out
    def fields(b):
        r=struct.unpack_from('<16Q',b,0);f,=struct.unpack_from('<Q',b,128);m,=struct.unpack_from('<I',b,136)
        x=[b[144+16*i:160+16*i] for i in range(16)];buf=b[400:656]
        return r,f,m,x,buf
    a=read(sys.argv[1]);b=read(sys.argv[2])
    print(len(a),len(b))
    cnt=collections.Counter();ex=collections.defaultdict(list);ndiff=0
    for (t,fa,ia,oa),(t2,fb,ib,ob) in zip(a,b):
        assert t==t2
        if ia!=ib: cnt['input:'+fa]+=1
        ra,ffa,ma,xa,ba=fields(oa);rb,ffb,mb,xb,bb=fields(ob)
        k=[]
        for i in range(16):
            if i in (4,15): continue
            if ra[i]!=rb[i]: k.append('r%d'%i)
        if (ffa&0x8d5)!=(ffb&0x8d5): k.append('flags')
        if (ma&~2)!=(mb&~2): k.append('mxcsr')
        elif ma!=mb: k.append('mxcsr.DE_only')
        for i in range(16):
            if xa[i]!=xb[i]: k.append('xmm')
        if ba!=bb: k.append('buffer')
        if k:
            ndiff+=1
            for kk in set(k): cnt[fa+':'+kk]+=1;
            if len(ex[fa])<3: ex[fa].append((t,sorted(set(k)),hex(ma),hex(mb)))
    print('cases differing:',ndiff)
    for k,v in cnt.most_common(30): print(v,k)
    for f,l in ex.items():
        for e in l: print(f,e)
else:
    import struct,sys,collections
    IO=200
    def read(p):
        d=open(p,'rb').read();o=0;n,=struct.unpack_from('<I',d,o);o+=4;out=[]
        for _ in range(n):
            l,=struct.unpack_from('<I',d,o);o+=4;code=d[o:o+l];o+=l
            l,=struct.unpack_from('<I',d,o);o+=4;t=d[o:o+l].decode();o+=l
            df,=struct.unpack_from('<Q',d,o);o+=8;i=d[o:o+IO];o+=IO;ou=d[o:o+IO];o+=IO
            out.append((code,t,df,i,ou))
        return out
    a=read(sys.argv[1]);b=read(sys.argv[2]);print(len(a),len(b))
    codes=sum(x[0]!=y[0] for x,y in zip(a,b));print('code bytes differ:',codes)
    ex=[x for x,y in zip(a,b) if x[0]!=y[0]][:2]
    for e in ex: print(' ',e[1],e[0].hex())
    for x,y in zip(a,b):
        if x[0]!=y[0]: print('  linux:',y[0].hex());break
    nd=0;c=collections.Counter();exs=[]
    for x,y in zip(a,b):
        ra=struct.unpack_from('<16Q',x[4]);rb=struct.unpack_from('<16Q',y[4])
        k=['r%d'%i for i in range(16) if i not in(4,15) and ra[i]!=rb[i]]
        fa,=struct.unpack_from('<Q',x[4],128);fb,=struct.unpack_from('<Q',y[4],128)
        if (fa&x[2])!=(fb&x[2]): k.append('flags')
        if x[4][136:]!=y[4][136:]: k.append('buffer')
        if x[3]!=y[3]: k.append('input')
        if k:
            nd+=1;c.update(k)
            if len(exs)<8: exs.append((x[1],k,hex(fa&x[2]),hex(fb&x[2])))
    print('cases differing:',nd,dict(c))
    for e in exs: print(e)
