import sys,struct,subprocess
BASE=0x08040000
fw=open(__import__("os").environ.get("FW","firmware/BLACKBOX-3.1.9.bin"),"rb").read()
def pools(val):
    b=struct.pack("<I",val); i=-1; r=[]
    while True:
        i=fw.find(b,i+1)
        if i<0: return r
        r.append(BASE+i)
def refs(pool):
    out=[]
    for a in range(max(BASE,pool-4096),pool,2):
        o=a-BASE; h=int.from_bytes(fw[o:o+2],"little")
        if h>>11==0b01001 and ((a+4)&~3)+(h&0xFF)*4==pool: out.append(a)
        h2=int.from_bytes(fw[o+2:o+4],"little")
        if h&0xFF7F==0xF85F and ((a+4)&~3)+(1 if h&0x80 else -1)*(h2&0xFFF)==pool: out.append(a)
    return out
for s in sys.argv[1:]:
    v=int(s,16)
    for p in pools(v):
        print(hex(v),"pool",hex(p),"refs",[hex(x) for x in refs(p)])
