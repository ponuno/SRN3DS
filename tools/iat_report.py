"""Read-only PE import/IAT inventory with Win32/D3DX shim coverage.

Uses only the standard library: the PE32/PE32+ import directory is decoded from
the data directory (index 1), including hint/name thunks, ordinal imports and
bound-import descriptors with a missing OriginalFirstThunk.

Coverage classes (per imported function):
  bound   - explicit resolver/binding table entry in source/win32_*.c
            (X("dll","name",...), Import("dll","name",...), C(...)/K(...)
            bodies, {"dll","name",fn} tables, stringified DEF(Name) tables) or
            an exported name quoted in source/d3dx_*.
  stub    - the name is mentioned somewhere in those sources (shim body,
            diagnostic, ordinal case) but has no explicit binding.
  missing - no mention at all.

Usage:
  python tools/iat_report.py [E:\\skyr\\TESV.exe] [--json] [--missing] [--strict]

--strict exits 1 when at least one import is missing (2 for a read/parse error).
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

DEFAULT_PE = r"E:\skyr\TESV.exe"
TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_@]*")


class PEError(Exception):
    pass


class PEImports:
    def __init__(self, path):
        self.path = str(path)
        self.data = Path(path).read_bytes()
        data = self.data
        if data[:2] != b"MZ":
            raise PEError("not an MZ executable")
        (pe_off,) = struct.unpack_from("<I", data, 0x3C)
        if data[pe_off:pe_off + 4] != b"PE\0\0":
            raise PEError("missing PE signature")
        machine, nsections, _, _, _, opt_size, _ = struct.unpack_from(
            "<HHIIIHH", data, pe_off + 4)
        opt = pe_off + 24
        (magic,) = struct.unpack_from("<H", data, opt)
        if magic == 0x10B:       # PE32
            self.bits = 32
            self.ptr = 4
            self.base = struct.unpack_from("<I", data, opt + 28)[0]
            dir_count_off, dir_off = opt + 92, opt + 96
        elif magic == 0x20B:     # PE32+
            self.bits = 64
            self.ptr = 8
            self.base = struct.unpack_from("<Q", data, opt + 24)[0]
            dir_count_off, dir_off = opt + 108, opt + 112
        else:
            raise PEError("unsupported optional header magic %#x" % magic)
        self.machine = machine
        (self.headers,) = struct.unpack_from("<I", data, opt + 60)
        self.sections = []
        for i in range(nsections):
            q = opt + opt_size + 40 * i
            name = data[q:q + 8].split(b"\0")[0].decode("ascii", "replace")
            vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, q + 8)
            self.sections.append((name, va, vsize, raw_off, raw_size))
        (n_dirs,) = struct.unpack_from("<I", data, dir_count_off)
        if n_dirs < 2:
            raise PEError("no import data directory")
        imp_rva, _ = struct.unpack_from("<II", data, dir_off + 8)
        if not imp_rva:
            raise PEError("empty import data directory")
        self.import_dir = imp_rva

    def rva_to_offset(self, rva, size=1):
        data = self.data
        for _, va, vsize, raw_off, raw_size in self.sections:
            if va <= rva < va + max(vsize, raw_size):
                off = raw_off + rva - va
                if off + size <= min(raw_off + raw_size, len(data)):
                    return off
                return None
        if rva < self.headers and rva + size <= len(data):
            return rva
        return None

    def cstring(self, rva):
        off = self.rva_to_offset(rva)
        if off is None:
            return None
        end = self.data.find(b"\0", off)
        if end < 0:
            return None
        return self.data[off:end].decode("ascii", "replace")

    def thunk(self, rva):
        off = self.rva_to_offset(rva, self.ptr)
        if off is None:
            raise PEError("thunk RVA %#x is not in a file-backed section" % rva)
        return int.from_bytes(self.data[off:off + self.ptr], "little")

    def imports(self):
        """Yield (dll, name, ordinal, hint, iat_rva) for every IAT slot."""
        off = self.rva_to_offset(self.import_dir, 20)
        if off is None:
            raise PEError("import directory RVA is not file-backed")
        flag = 1 << (self.ptr * 8 - 1)
        while off is not None and off + 20 <= len(self.data):
            oft, stamp, fwd, name_rva, ft = struct.unpack_from("<IIIII", self.data, off)
            if not (oft or stamp or fwd or name_rva or ft):
                break
            if not ft:
                raise PEError("import descriptor has no FirstThunk")
            dll = self.cstring(name_rva) or "<unknown>"
            # Bound imports may leave OriginalFirstThunk at zero; the IAT then
            # doubles as the lookup table.
            lookup = oft or ft
            index = 0
            while True:
                if index > 1_000_000:
                    raise PEError("thunk table does not terminate")
                try:
                    value = self.thunk(lookup + index * self.ptr)
                except PEError:
                    break
                if not value:
                    break
                hint, ordinal, name = None, None, None
                if value & flag:
                    ordinal = value & 0xFFFF
                    name = "#%u" % ordinal
                else:
                    name_off = self.rva_to_offset(value, 2)
                    if name_off is not None:
                        (hint,) = struct.unpack_from("<H", self.data, name_off)
                        name = self.cstring(value + 2)
                    if not name:
                        name = "<unresolved thunk>"
                yield dll, name, ordinal, hint, ft + index * self.ptr
                index += 1
            off += 20


def norm_dll(dll):
    return dll.lower()[:-4] if dll.lower().endswith(".dll") else dll.lower()


class ShimIndex:
    def __init__(self, source):
        win = sorted(Path(source).glob("win32_*.c"))
        d3dx = sorted(Path(source).glob("d3dx_*"))
        if not win and not d3dx:
            raise ValueError("no win32_*.c / d3dx_* sources in %s" % source)
        win_files = [(p, p.read_text(encoding="utf-8", errors="replace")) for p in win]
        win_text = "\n".join(text for _, text in win_files)
        d3dx_text = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in d3dx if p.is_file())
        all_text = win_text + "\n" + d3dx_text
        self.mentions = set(TOKEN.findall(all_text))
        self.mentions_lower = {n.lower() for n in self.mentions}
        self.bindings = set()
        # Ordinals are only meaningful together with their DLL: win32_input.c
        # mentions XINPUT #2/#3/#4, which must not mark WSOCK32 #2/#3/#4 as
        # covered.
        self.ord_by_dll = set()
        for _, text in win_files:
            dlls = {norm_dll(d) for d in re.findall(r"\b([A-Za-z0-9_]+\.dll)\b", text)}
            ords = {int(n) for n in re.findall(r"#(\d+)", text)}
            for dll in dlls:
                for ordinal in ords:
                    self.ord_by_dll.add((dll, ordinal))

        # #define DEF(X) { "kernel32.dll", #X, s_##X },  -> DEF(Name) calls
        for m in re.finditer(
                r'#define\s+(\w+)\s*\(\s*(\w+)\s*\)\s*\{\s*"([^"]+)"\s*,\s*#\2\b', win_text):
            macro, dll = m.group(1), m.group(3)
            for call in re.finditer(
                    r'(?<![\w#])' + re.escape(macro) + r'\s*\(\s*([A-Za-z_]\w*)\s*\)', win_text):
                self.bindings.add((norm_dll(dll), call.group(1)))

        # X(...) inside K(...)/C(...)/BINK(...) tables, Import(...) tables, and
        # plain {"dll", "name", fn} initializer tables.
        for dll, name in re.findall(
                r'\b(?:X|Import)\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"', win_text):
            self.bindings.add((norm_dll(dll), name))
        for dll, name in re.findall(
                r'\{\s*"([^"]+\.dll)"\s*,\s*"([^"]+)"\s*,', win_text):
            self.bindings.add((norm_dll(dll), name))

        # Resolver tables in d3dx_* name exports as exact quoted strings.
        self.d3dx_exports = {n for n in re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', d3dx_text)
                             if n.startswith("D3DX")}
        self.bindings_lower = {(d, n.lower()) for d, n in self.bindings}

    def classify(self, dll, name):
        d = norm_dll(dll)
        if (d, name.lower()) in self.bindings_lower:
            return "bound"
        if name in self.d3dx_exports and d.startswith("d3dx9_42"):
            return "bound"
        if name.startswith("#"):
            return "stub" if (d, int(name[1:])) in self.ord_by_dll else "missing"
        return "stub" if name.lower() in self.mentions_lower else "missing"


def build_report(pe, shims):
    dlls = []
    totals = {"bound": 0, "stub": 0, "missing": 0}
    for dll, name, ordinal, hint, iat_rva in pe.imports():
        entry = {
            "name": name,
            "status": shims.classify(dll, name),
            "ordinal": ordinal,
            "hint": hint,
            "iat_rva": "0x%08x" % iat_rva,
            "iat_va": "0x%08x" % (pe.base + iat_rva),
        }
        if not dlls or dlls[-1]["name"] != dll:
            dlls.append({"name": dll, "imports": [],
                         "counts": {"bound": 0, "stub": 0, "missing": 0}})
        dlls[-1]["imports"].append(entry)
        dlls[-1]["counts"][entry["status"]] += 1
        totals[entry["status"]] += 1
    for d in dlls:
        d["counts"]["total"] = len(d["imports"])
    totals["total"] = sum(totals.values())
    return {
        "pe": pe.path,
        "machine": "0x%04x" % pe.machine,
        "image_base": "0x%08x" % pe.base,
        "pointer_size": pe.ptr,
        "counts": totals,
        "dlls": dlls,
    }


def print_text(report, only_missing):
    c = report["counts"]
    print("PE: %s  machine=%s  image_base=%s" %
          (report["pe"], report["machine"], report["image_base"]))
    print("TOTAL imports=%d bound=%d stub=%d missing=%d" %
          (c["total"], c["bound"], c["stub"], c["missing"]))
    for dll in report["dlls"]:
        dc = dll["counts"]
        shown = [i for i in dll["imports"]
                 if not only_missing or i["status"] == "missing"]
        if not shown:
            continue
        print("\n%s: total=%d bound=%d stub=%d missing=%d" %
              (dll["name"], dc["total"], dc["bound"], dc["stub"], dc["missing"]))
        for imp in shown:
            extra = " ordinal=%u" % imp["ordinal"] if imp["ordinal"] is not None else ""
            print("  %-7s %-38s IAT rva=%s va=%s%s" %
                  (imp["status"], imp["name"], imp["iat_rva"], imp["iat_va"], extra))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pe", nargs="?", default=DEFAULT_PE, help="PE image to inspect")
    ap.add_argument("--json", action="store_true", help="emit the full report as JSON")
    ap.add_argument("--missing", action="store_true", help="only print missing imports")
    ap.add_argument("--strict", action="store_true", help="exit 1 if any import is missing")
    ap.add_argument("--source", default=None,
                    help="directory with win32_*.c / d3dx_* (default: ../source)")
    args = ap.parse_args(argv)
    source = args.source or Path(__file__).resolve().parent.parent / "source"
    try:
        report = build_report(PEImports(args.pe), ShimIndex(source))
    except (OSError, ValueError, PEError) as exc:
        print("iat_report: %s" % exc, file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_text(report, args.missing)
    return 1 if args.strict and report["counts"]["missing"] else 0


if __name__ == "__main__":
    sys.exit(main())
