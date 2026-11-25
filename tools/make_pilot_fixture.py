"""Create our own minimal PE32 matrix-scaling DLL for offline tests."""
import struct
from pathlib import Path
import sys

def fixture():
    b=bytearray(0x600)
    def w16(p,v): struct.pack_into('<H',b,p,v)
    def w32(p,v): struct.pack_into('<I',b,p,v)
    w16(0,0x5a4d);w32(0x3c,0x80);b[0x80:0x84]=b'PE\0\0'
    w16(0x84,0x14c);w16(0x86,2);w16(0x94,224);w16(0x96,0x2102)
    o=0x98;w16(o,0x10b);w32(o+4,0x200);w32(o+8,0x200);w32(o+20,0x1000);w32(o+24,0x2000)
    w32(o+28,0x08000000);w32(o+32,0x1000);w32(o+36,0x200);w16(o+40,4);w16(o+48,4)
    w32(o+56,0x3000);w32(o+60,0x200);w16(o+68,3);w32(o+72,0x100000);w32(o+76,0x1000)
    w32(o+80,0x100000);w32(o+84,0x1000);w32(o+92,16);w32(o+96,0x2000);w32(o+100,0x100)
    for i,(name,rva,raw,flags) in enumerate([(b'.text',0x1000,0x200,0x60000020),(b'.edata',0x2000,0x400,0x40000040)]):
        s=o+224+i*40;b[s:s+len(name)]=name;w32(s+8,0x200);w32(s+12,rva);w32(s+16,0x200);w32(s+20,raw);w32(s+36,flags)
    code=bytes.fromhex('8b442404 31d2')+b''.join(bytes([0x89,0x50,i*4]) for i in range(16))
    code+=bytes.fromhex('8b4c2408 8908 8b4c240c 894814 8b4c2410 894828 c7403c0000803f c21000')
    b[0x200:0x200+len(code)]=code
    for p,v in [(12,0x2060),(16,1),(20,1),(24,1),(28,0x2040),(32,0x2044),(36,0x2048),(0x40,0x1000),(0x44,0x2080)]:w32(0x400+p,v)
    for p,s in [(0x460,b'fixture.dll\0'),(0x480,b'D3DXMatrixScaling\0')]:b[p:p+len(s)]=s
    return b

if __name__=='__main__':
    with Path(sys.argv[1]).open('xb') as f:f.write(fixture())
