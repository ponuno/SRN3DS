"""Create a tiny self-authored x86 PE32 arithmetic test for lift.py."""
from pathlib import Path
import struct
import json
import sys

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=False)
image = bytearray(0x400)
def w16(p,v): struct.pack_into('<H',image,p,v)
def w32(p,v): struct.pack_into('<I',image,p,v)
w16(0,0x5a4d);w32(0x3c,0x80)
image[0x80:0x84]=b'PE\0\0'
w16(0x84,0x14c);w16(0x86,1);w16(0x94,224);w16(0x96,0x102)
o=0x98
w16(o,0x10b);w32(o+4,0x200);w32(o+16,0x1000);w32(o+20,0x1000)
w32(o+28,0x400000);w32(o+32,0x1000);w32(o+36,0x200);w16(o+40,4)
w16(o+48,4);w32(o+56,0x2000);w32(o+60,0x200);w16(o+68,3)
w32(o+72,0x100000);w32(o+76,0x1000);w32(o+80,0x100000);w32(o+84,0x1000);w32(o+92,16)
s=o+224
image[s:s+8]=b'.text\0\0\0';w32(s+8,0x200);w32(s+12,0x1000);w32(s+16,0x200);w32(s+20,0x200);w32(s+36,0x60000020)
# int add(int a,int b) { return a+b; }
add=bytes.fromhex('8b 44 24 04 03 44 24 08 c3')
# int stdcall add5(int *p) { lock add [p],5; return *p; }
add5=bytes.fromhex('8b 4c 24 04 f0 83 01 05 8b 01 c2 04 00')
image[0x200:0x200+len(add)]=add
image[0x210:0x210+len(add5)]=add5
(out/'fixture.exe').write_bytes(image)
(out/'functions.json').write_text(json.dumps({'iat':{},'jump_tables':{},'functions':{
    '401000':[[0x401000,0x401000+len(add)]], '401010':[[0x401010,0x401010+len(add5)]]}},indent=2),encoding='utf-8')
print('Created arithmetic-only fixture (no game data) at',out)
