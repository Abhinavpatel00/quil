import zlib,struct,sys
def load(p):
    d=open(p,'rb').read(); pos=8; w=h=None; idat=b''
    while pos<len(d):
        ln=struct.unpack('>I',d[pos:pos+4])[0]; typ=d[pos+4:pos+8]; data=d[pos+8:pos+8+ln]
        if typ==b'IHDR': w,h=struct.unpack('>II',data[:8])
        elif typ==b'IDAT': idat+=data
        pos+=12+ln
    raw=zlib.decompress(idat); bpp=4; stride=w*bpp
    out=bytearray(); prev=bytearray(stride); i=0
    for y in range(h):
        f=raw[i]; i+=1; line=bytearray(raw[i:i+stride]); i+=stride
        if f==1:
            for x in range(bpp,stride): line[x]=(line[x]+line[x-bpp])&255
        elif f==2:
            for x in range(stride): line[x]=(line[x]+prev[x])&255
        elif f==3:
            for x in range(stride): line[x]=(line[x]+((line[x-bpp] if x>=bpp else 0)+prev[x])//2)&255
        elif f==4:
            def pa(a,b,c):
                p=a+b-c; x,y,z=abs(p-a),abs(p-b),abs(p-c)
                return a if x<=y and x<=z else (b if y<=z else c)
            for x in range(stride):
                a=line[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
                line[x]=(line[x]+pa(a,b,c))&255
        out+=line; prev=line
    return w,h,bytes(out)
w,h,a=load(sys.argv[1]); _,_,b=load(sys.argv[2])
diff=sum(1 for i in range(0,len(a),4) if a[i:i+4]!=b[i:i+4])
print(f"{diff} differing pixels of {len(a)//4}")
