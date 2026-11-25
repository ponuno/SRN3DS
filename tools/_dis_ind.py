import capstone, pefile, json, sys
p = pefile.PE(r"E:\skyr\TESV.exe")
base = p.OPTIONAL_HEADER.ImageBase
img = p.get_memory_mapped_image()
data = json.load(open(r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json", encoding="utf-8"))
iat = {int(k, 16): v for k, v in data["iat"].items()}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
target = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0xf5204f
start = target - 0x80
for i in md.disasm(img[start-base:target-base+0x10], start):
    note = ""
    for op in i.operands:
        if op.type == capstone.x86.X86_OP_MEM and not op.mem.base and not op.mem.index:
            slot = op.mem.disp & 0xffffffff
            if slot in iat: note = "  ; " + iat[slot]
    mark = "  <== fault" if i.address == target else ""
    print(hex(i.address), i.mnemonic, i.op_str, note, mark)
