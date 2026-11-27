"""Scan the GFx9 code window for D3D9 device-vtable call sites.

GFx9 loads the device vtable once per call:
    mov  eax, [ebx+0x1ac]     ; pDevice
    mov  edx, [eax]           ; vtable
    push ...
    mov  eax, [edx+0x16c]     ; slot
    call eax

The scanner tracks which register holds a "vtable" (loaded from [obj]) and
then resolves `mov r,[vtbl+disp] ; call r` to a DEVICE slot name.

Usage: python tools/gfx_calls.py [lo] [hi]
"""
import struct
import sys
from pathlib import Path

IMAGE = Path(r"E:\census_ir2\ir_container\_work\ir_container_input.bin")

DEVICE = [
    "QueryInterface", "AddRef", "Release", "TestCooperativeLevel",
    "GetAvailableTextureMem", "EvictManagedResources", "GetDirect3D",
    "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
    "SetCursorProperties", "SetCursorPosition", "ShowCursor",
    "CreateAdditionalSwapChain", "GetSwapChain", "GetNumberOfSwapChains",
    "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetDialogBoxMode",
    "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture",
    "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer",
    "CreateRenderTarget", "CreateDepthStencilSurface", "UpdateSurface",
    "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData", "StretchRect",
    "ColorFill", "CreateOffscreenPlainSurface", "SetRenderTarget",
    "GetRenderTarget", "SetDepthStencilSurface", "GetDepthStencilSurface",
    "BeginScene", "EndScene", "Clear", "SetTransform", "GetTransform",
    "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial",
    "GetMaterial", "SetLight", "GetLight", "LightEnable", "GetLightEnable",
    "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState",
    "CreateStateBlock", "BeginStateBlock", "EndStateBlock", "SetClipStatus",
    "GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState",
    "SetTextureStageState", "GetSamplerState", "SetSamplerState",
    "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries",
    "SetCurrentTexturePalette", "GetCurrentTexturePalette", "SetScissorRect",
    "GetScissorRect", "SetSoftwareVertexProcessing",
    "GetSoftwareVertexProcessing", "SetNPatchMode", "GetNPatchMode",
    "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
    "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexDeclaration",
    "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF", "GetFVF",
    "CreateVertexShader", "SetVertexShader", "GetVertexShader",
    "SetVertexShaderConstantF", "GetVertexShaderConstantF",
    "SetVertexShaderConstantI", "GetVertexShaderConstantI",
    "SetVertexShaderConstantB", "GetVertexShaderConstantB",
    "SetStreamSource", "GetStreamSource", "SetStreamSourceFreq",
    "GetStreamSourceFreq", "SetIndices", "GetIndices", "CreatePixelShader",
    "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF",
    "GetPixelShaderConstantF", "SetPixelShaderConstantI",
    "GetPixelShaderConstantI", "SetPixelShaderConstantB",
    "GetPixelShaderConstantB", "DrawRectPatch", "DrawTriPatch", "DeletePatch",
    "CreateQuery",
]


def load():
    data = IMAGE.read_bytes()
    magic, lo, size, entry = struct.unpack("<IIII", data[:16])
    if magic != 0x49524D31:
        sys.exit("not an IRM1 container input")
    return data[16:], lo, size


def main(argv):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_OP_IMM

    code, lo, size = load()
    start = int(argv[1], 16) if len(argv) > 1 else 0xF80000
    end = int(argv[2], 16) if len(argv) > 2 else 0xF8D000
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    insns = list(md.disasm(code[start - lo:end - lo], start))

    # reg -> "obj" (pointer to a COM object) or ("vslot", disp)
    obj = set()
    objoff = {}
    vtbl = {}
    hits = {}
    for i, ins in enumerate(insns):
        m = ins.mnemonic
        ops = ins.operands
        if m == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG \
                and ops[1].type == X86_OP_MEM:
            dst = ins.reg_name(ops[0].reg)
            mem = ops[1].mem
            src = ins.reg_name(mem.base) if mem.base else None
            if src and mem.index == 0 and mem.disp == 0 and src in obj:
                vtbl[dst] = True            # [obj] -> vtable ptr
                obj.discard(dst)
                continue
            if src and src in vtbl and mem.index == 0:
                vtbl[dst] = ("slot", mem.disp, i, objoff.get(src))   # [vtbl+disp] -> method
                continue
            # plain field load: possible object pointer
            if mem.base and mem.index == 0 and mem.disp != 0:
                obj.add(dst); objoff[dst] = mem.disp
            else:
                obj.discard(dst)
            vtbl[dst] = None
            continue
        if m == "push" and ops[0].type == X86_OP_MEM:
            mem = ops[0].mem
            if mem.base and mem.disp == 0 and mem.index == 0:
                if vtbl.get(ins.reg_name(mem.base)):
                    # vtable pushed as a call target slot? no op
                    pass
            continue
        if m == "call":
            op = ops[0]
            target = None
            if op.type == X86_OP_REG:
                r = ins.reg_name(op.reg)
                info = vtbl.get(r)
                if isinstance(info, tuple):
                    disp = info[1]
                    if 0 <= disp // 4 < len(DEVICE):
                        target = DEVICE[disp // 4]
            elif op.type == X86_OP_MEM and op.mem.base == 0:
                target = "*iat"
            if target and target != "*iat":
                hits.setdefault(target, []).append(ins.address)
                info = vtbl[ins.reg_name(op.reg)]
                print("%08x  %-28s +0x%-4x obj=+0x%x" % (
                    ins.address, target, info[1],
                    info[3] if len(info) > 3 and info[3] is not None else 0))
            elif target == "*iat":
                va = op.mem.disp
                hits.setdefault("@%08x" % va, []).append(ins.address)
            # a call clobbers volatile regs
            if op.type == X86_OP_REG:
                vtbl.pop(ins.reg_name(op.reg), None)
            for r in ("eax", "ecx", "edx"):
                vtbl.pop(r, None)
            continue
        # any other write to a tracked reg invalidates it
        if ops and ops[0].type == X86_OP_REG:
            name = ins.reg_name(ops[0].reg)
            if name in vtbl:
                vtbl[name] = None

    print("\n== D3D9 device calls")
    for name, lst in sorted(hits.items(), key=lambda kv: -len(kv[1])):
        if name.startswith("@"):
            print("  IAT %s  x%d  %s" % (name[1:], len(lst),
                                         ",".join("%x" % a for a in lst[:4])))
            continue
        print("  %-28s x%-3d %s" % (name, len(lst),
                                    ",".join("%x" % a for a in lst[:8])))


if __name__ == "__main__":
    main(sys.argv)
