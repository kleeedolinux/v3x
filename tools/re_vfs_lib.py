"""Shared helpers for the VFS / content-pack RE tools (re_vfs_*.py).
Mirrors the V3X runtime API: find_string, find_xref (RIP-relative lea/mov), function_start (.pdata).
Usage as a script: re_vfs_lib.py <exe> dis <rva-hex> [max_insns]   disassemble the function containing rva
                   re_vfs_lib.py <exe> calls <rva-hex>              list direct call targets of that function
                   re_vfs_lib.py <exe> callers <rva-hex>            list direct E8 callers of an rva"""
import bisect, re, struct, sys
import pefile, capstone

DEFAULT_EXE = r'D:\SteamLibrary\steamapps\common\Victoria 3\binaries\victoria3.exe'


class Image:
    def __init__(self, exe=DEFAULT_EXE):
        self.pe = pefile.PE(exe, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.text = next(s for s in self.pe.sections if s.Name.startswith(b'.text'))
        self.tdata = self.text.get_data()
        self.tva = self.text.VirtualAddress
        self._funcs = None
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = False

    # ---- strings / xrefs -------------------------------------------------
    def find_string(self, s):
        """All RVAs of the exact NUL-terminated string (preceded by NUL or section start)."""
        needle = (s.encode() if isinstance(s, str) else s) + b'\0'
        out = []
        for sec in self.pe.sections:
            d = sec.get_data()
            for m in re.finditer(re.escape(needle), d):
                if m.start() == 0 or d[m.start() - 1] == 0:
                    out.append(sec.VirtualAddress + m.start())
        return out

    _PAT = re.compile(rb'[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)

    def xrefs(self, target):
        """RIP-relative lea/mov r64 that resolve to target (same matcher as src/core.c find_xref)."""
        res = []
        d = self.tdata
        for m in self._PAT.finditer(d):
            i = m.start()
            if i + 7 > len(d):
                continue
            disp = struct.unpack_from('<i', d, i + 3)[0]
            if self.tva + i + 7 + disp == target:
                res.append(self.tva + i)
        return res

    def callers(self, target):
        """Direct E8 rel32 calls to target."""
        res = []
        d = self.tdata
        for m in re.finditer(rb'\xe8', d):
            i = m.start()
            if i + 5 > len(d):
                continue
            if self.tva + i + 5 + struct.unpack_from('<i', d, i + 1)[0] == target:
                res.append(self.tva + i)
        return res

    # ---- functions (.pdata) ----------------------------------------------
    def funcs(self):
        if self._funcs is None:
            pdata = next(s for s in self.pe.sections if s.Name.startswith(b'.pdata'))
            raw = pdata.get_data()
            ents = []
            for i in range(0, len(raw) - 11, 12):
                b, e, u = struct.unpack_from('<III', raw, i)
                if b == 0:
                    break
                ents.append((b, e, u))
            ents.sort()
            self._funcs = ents
            self._starts = [x[0] for x in ents]
        return self._funcs

    def _entry(self, rva):
        f = self.funcs()
        i = bisect.bisect_right(self._starts, rva) - 1
        if i < 0 or not (f[i][0] <= rva < f[i][1]):
            return None
        return f[i]

    def function_start(self, rva):
        """Like src/core.c: follow chained unwind info (UNW_FLAG_CHAININFO) back to the primary entry."""
        e = self._entry(rva)
        if not e:
            return None
        for _ in range(32):
            b, end, u = e
            uw = self.pe.get_data(u & ~1 if u & 1 else u, 4)
            ver_flags, _, count, _ = uw
            if not (ver_flags >> 3) & 4:  # UNW_FLAG_CHAININFO
                return b
            off = (u + 4 + ((count + 1) & ~1) * 2)
            b2, e2, u2 = struct.unpack('<III', self.pe.get_data(off, 12))
            e = (b2, e2, u2)
        return e[0]

    def function_range(self, rva):
        e = self._entry(rva)
        return (e[0], e[1]) if e else None

    # ---- disassembly -----------------------------------------------------
    def code(self, rva, n):
        off = rva - self.tva
        return self.tdata[off:off + n]

    def disasm(self, rva, n=0x400, max_insns=4000):
        return list(self.md.disasm(self.code(rva, n), rva))[:max_insns]

    def func_insns(self, rva):
        """Instructions of the primary function plus its chained fragments are not merged;
        this disassembles the single .pdata range that contains rva."""
        r = self.function_range(rva)
        if not r:
            return []
        return list(self.md.disasm(self.code(r[0], r[1] - r[0]), r[0]))

    def call_targets(self, rva):
        out = []
        for ins in self.func_insns(rva):
            if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
                out.append((ins.address, int(ins.op_str, 16)))
        return out

    def string_at(self, rva, maxlen=200):
        try:
            b = self.pe.get_data(rva, maxlen)
        except Exception:
            return None
        z = b.find(b'\0')
        if z <= 0:
            return None
        s = b[:z]
        if all(32 <= c < 127 for c in s):
            return s.decode()
        return None

    def annotate(self, ins):
        """Return string literal referenced by a rip-relative operand, if any."""
        m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', ins.op_str)
        if not m:
            return None
        disp = int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
        tgt = ins.address + ins.size + disp
        s = self.string_at(tgt)
        return (tgt, s)


def main(argv):
    img = Image(argv[1])
    cmd, rva = argv[2], int(argv[3], 16)
    if cmd == 'dis':
        mx = int(argv[4]) if len(argv) > 4 else 100000
        st = img.function_start(rva)
        r = img.function_range(rva)
        print(f'; function_start={st:#x} pdata range={r[0]:#x}-{r[1]:#x}')
        for ins in img.func_insns(rva)[:mx]:
            a = img.annotate(ins)
            extra = ''
            if a:
                extra = f'   ; {a[0]:#x}' + (f' "{a[1]}"' if a[1] else '')
            print(f'{ins.address:08x}: {ins.mnemonic} {ins.op_str}{extra}')
    elif cmd == 'calls':
        for at, t in img.call_targets(rva):
            print(f'{at:08x} -> {t:08x}')
    elif cmd == 'callers':
        for c in img.callers(rva):
            st = img.function_start(c)
            print(f'{c:08x} in fn {st:#x}' if st else f'{c:08x}')


if __name__ == '__main__':
    main(sys.argv)
