"""Read-only PE32 metadata report; no game bytes are copied to output."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

MODULES = ('TESV.exe','d3dx9_42.dll','binkw32.dll','X3DAudio1_7.dll','steam_api.dll')

def inspect_pe(path):
    data=path.read_bytes()
    def u16(p): return struct.unpack_from('<H',data,p)[0]
    def u32(p): return struct.unpack_from('<I',data,p)[0]
    if data[:2]!=b'MZ': raise ValueError('Missing DOS signature')
    h=u32(0x3c)
    if data[h:h+4]!=b'PE\0\0' or u16(h+4)!=0x14c: raise ValueError('Expected x86 PE')
    o=h+24; osize=u16(h+20); n=u16(h+6)
    if osize<96 or u16(o)!=0x10b or n>96: raise ValueError('Invalid PE32 header')
    sections=[]
    for i in range(n):
        s=o+osize+i*40
        sections.append((u32(s+12),u32(s+8),u32(s+20),u32(s+16),u32(s+36)))
    def raw(rva,n):
        if rva>=0 and rva+n<=u32(o+60) and rva+n<=len(data): return rva
        for va,vs,p,rs,flags in sections:
            if va<=rva and rva-va+n<=rs and p+rva-va+n<=len(data):return p+rva-va
        raise ValueError(f'RVA not file-backed: {rva:#x}')
    def word(rva):return u16(raw(rva,2))
    def dword(rva):return u32(raw(rva,4))
    def string(rva):
        b=bytearray()
        for i in range(512):
            v=data[raw(rva+i,1)]
            if not v:return b.decode('ascii','replace')
            b.append(v)
        raise ValueError('Unterminated PE string')
    def directory(i):
        if i>=u32(o+92) or 96+8*(i+1)>osize:return 0,0
        return u32(o+96+8*i),u32(o+100+8*i)
    imports=[];exports={}
    rva,size=directory(1)
    if rva and size:
        for i in range(min(size//20,1024)):
            d=rva+20*i; fields=[dword(d+4*j) for j in range(5)]
            if not any(fields):break
            ilt,_,_,name,iat=fields;dll=string(name).lower();thunk=ilt or iat
            for j in range(8192):
                v=dword(thunk+4*j)
                if not v:break
                imports.append({'dll':dll,'name':f'#{v&0xffff}' if v&0x80000000 else string(v+2)})
            else:raise ValueError('Import thunk limit exceeded')
        else:raise ValueError('Import descriptor terminator missing')
    rva,size=directory(0)
    if rva and size:
        base,nf,nn,ft,nt,ot=[dword(rva+p) for p in (16,20,24,28,32,36)]
        if nf>65536 or nn>65536:raise ValueError('Export limit exceeded')
        for i in range(nf):
            v=dword(ft+i*4)
            if v:exports[f'#{base+i}']='forwarder' if rva<=v<rva+size else 'local'
        for i in range(nn):
            idx=word(ot+2*i)
            if idx>=nf:raise ValueError('Invalid export ordinal')
            k=f'#{base+idx}'
            if k in exports:exports[string(dword(nt+i*4))]=exports[k]
    return {'file':path.name,'sha256':hashlib.sha256(data).hexdigest(),'file_bytes':len(data),
            'image_base':hex(u32(o+28)),'image_bytes':u32(o+56),'entry_rva':hex(u32(o+16)),
            'executable_bytes':sum(max(vs,rs) for _,vs,_,rs,f in sections if f&0x20000000),
            'imports':imports,'exports':exports,'delay_import_directory_present':bool(directory(13)[0])}

def audit_shim_sources(src_dir):
    """Every registry that can resolve an import: core X(...) plus D(...) modules."""
    paths=[src_dir/'win32_shims.c']+sorted(src_dir.glob('win32_*.c'))
    seen=set()
    for p in paths:
        if p in seen or not p.is_file():continue
        seen.add(p)
        yield p.read_text(encoding='utf-8')

def audit(folder,exe_override=None):
    files={p.name.lower():p for p in folder.iterdir() if p.is_file()}
    if exe_override is not None:
        files['tesv.exe']=exe_override
    modules={};errors={}
    for name in MODULES:
        key=name.lower()
        try:modules[key]=inspect_pe(files[key])
        except (KeyError,ValueError,OSError,struct.error) as e:errors[name]=str(e) if key in files else 'not found'
    src_dir=Path(__file__).resolve().parents[1]/'source'
    shims=set()
    for text in audit_shim_sources(src_dir):
        shims.update((d.lower(),n) for d,n in re.findall(r'X\("([^"]+)",\s*"([^"]+)"',text))
        shims.update((d.lower(),n) for d,n in re.findall(r'D\("([^"]+)",\s*"([^"]+)"',text))
        shims.update((d.lower(),n) for d,n in re.findall(r'\{\s*"([^"]+\.dll)",\s*"([^"]+)"',text))
        # DEF(Name) tables expand to { "dll", #Name, fn }
        for dll in re.findall(r'#define\s+DEF\(\w+\)\s*\{\s*"([^"]+)"',text):
            for name in re.findall(r'\bDEF\((\w+)\)',text):
                shims.add((dll.lower(),name))
    missing=Counter();counts=Counter()
    for m in modules.values():
        for imp in m['imports']:
            dll,name=imp['dll'],imp['name'];local=modules.get(dll,{}).get('exports',{}).get(name)
            status='shim' if (dll.lower(),name) in shims else 'local_export_not_yet_lifted' if local=='local' else 'graphics_bridge' if (dll,name)==('d3d9.dll','Direct3DCreate9') else 'unresolved'
            imp['status']=status;counts[status]+=1
            if status=='unresolved':missing[(dll,name)]+=1
    return {'modules':modules,'errors':errors,'total_image_bytes':sum(m['image_bytes'] for m in modules.values()),
            'import_counts':dict(counts),'unresolved':[{'dll':d,'name':n,'references':c} for (d,n),c in sorted(missing.items(),key=lambda x:(-x[1],x[0]))],
            'limitations':'Static regular imports only; delay imports, dynamic resolution, generated ARM code, resources and runtime heap are not included. Shim presence is not proof of correctness.'}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('game_dir',type=Path);p.add_argument('--output',type=Path,default=Path('skyrim_audit.json'))
    p.add_argument('--exe',type=Path,help='override TESV.exe with another local copy (e.g. an unprotected build) while using game_dir for dependencies')
    a=p.parse_args()
    report=audit(a.game_dir,a.exe)
    with a.output.open('x',encoding='utf-8') as f:json.dump(report,f,indent=2)
    print(f"PE images: {report['total_image_bytes']/1048576:.2f} MiB (not total RAM)")
    print(report['import_counts']);print('Missing/unreadable:',report['errors']);print('Report:',a.output)
