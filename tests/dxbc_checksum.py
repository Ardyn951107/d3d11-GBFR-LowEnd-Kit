import struct, math
S=[7,12,17,22]*4+[5,9,14,20]*4+[4,11,16,23]*4+[6,10,15,21]*4
K=[int(abs(math.sin(i+1))*2**32)&0xffffffff for i in range(64)]
def rol(x,c): return ((x<<c)|(x>>(32-c)))&0xffffffff
def block(st,b):
    M=struct.unpack('<16I',b); a,bb,c,d=st
    for i in range(64):
        if i<16: f=(bb&c)|(~bb&d); g=i
        elif i<32: f=(d&bb)|(~d&c); g=(5*i+1)%16
        elif i<48: f=bb^c^d; g=(3*i+5)%16
        else: f=c^(bb|~d); g=(7*i)%16
        f=(f+a+K[i]+M[g])&0xffffffff; a,d,c=d,c,bb; bb=(bb+rol(f,S[i]))&0xffffffff
    return [(x+y)&0xffffffff for x,y in zip(st,[a,bb,c,d])]
def checksum(d):
    data=d[20:]; n=len(data); bits=n*8
    st=[0x67452301,0xefcdab89,0x98badcfe,0x10325476]
    full=n-(n%64)
    for i in range(0,full,64): st=block(st,data[i:i+64])
    last=data[full:]
    if len(last)>=56:
        st=block(st,(last+b'\x80').ljust(64,b'\0'))
        st=block(st,struct.pack('<I',bits)+b'\0'*56+struct.pack('<I',(bits>>2)|1))
    else:
        st=block(st,(struct.pack('<I',bits)+last+b'\x80').ljust(60,b'\0')+struct.pack('<I',(bits>>2)|1))
    return struct.pack('<4I',*st)
