"""re-sim helper library: one-time xref index (RIP-relative lea/mov + any rip-relative disp32
in common opcodes) cached to a pickle, .pdata function table, string lookup, disassembly.
Usage as CLI:
  re_sim_lib.py <exe> str TEXT...        -> xrefs + enclosing function of each string
  re_sim_lib.py <exe> rva HEX...         -> xrefs to RVA
  re_sim_lib.py <exe> dis HEX [N]        -> disassemble N instructions from RVA
  re_sim_lib.py <exe> fn HEX             -> function containing RVA (start,end)
  re_sim_lib.py <exe> calls HEX          -> direct call targets inside function containing RVA
  re_sim_lib.py <exe> callers HEX        -> direct call sites (E8) targeting RVA
"""
import bisect, os, pickle, re, struct, sys, tempfile
import pefile, capstone

CACHE = os.environ.get('RE_SIM_CACHE', os.path.join(os.path.dirname(os.path.abspath(__file__)), 're_sim_index.pkl'))
# lea/mov reg, [rip+disp32] with REX.W (48/4C) -> disp at +3, len 7
PAT = re.compile(rb'[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
CALL = re.compile(rb'\xe8', re.S)


class Img:
    def __init__(self, exe):
        self.pe = pe = pefile.PE(exe, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.secs = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
        self.text = self.secs['.text']
        self.tdata = self.text.get_data()
        self.tva = self.text.VirtualAddress
        st = os.stat(exe)
        key = (exe, st.st_size, int(st.st_mtime))
        if os.path.exists(CACHE):
            with open(CACHE, 'rb') as f:
                c = pickle.load(f)
            if c.get('key') == key:
                self.__dict__.update(c['d'])
                return
        self._build()
        with open(CACHE, 'wb') as f:
            pickle.dump({'key': key, 'd': {k: getattr(self, k) for k in
                                           ('xref', 'fstart', 'fend', 'callsto')}}, f)

    def _build(self):
        d, va = self.tdata, self.tva
        xref = {}
        for m in PAT.finditer(d):
            i = m.start()
            t = va + i + 7 + struct.unpack_from('<i', d, i + 3)[0]
            xref.setdefault(t, []).append(va + i)
        self.xref = xref
        pd = self.secs['.pdata'].get_data()
        fs, fe = [], []
        for i in range(0, len(pd) - 11, 12):
            b, e, _ = struct.unpack_from('<III', pd, i)
            if b == 0:
                break
            fs.append(b); fe.append(e)
        order = sorted(range(len(fs)), key=lambda k: fs[k])
        self.fstart = [fs[k] for k in order]; self.fend = [fe[k] for k in order]
        callsto = {}
        lo, hi = va, va + len(d)
        for m in CALL.finditer(d):
            i = m.start()
            if i + 5 > len(d):
                break
            t = va + i + 5 + struct.unpack_from('<i', d, i + 1)[0]
            if lo <= t < hi and t in self._fset():
                callsto.setdefault(t, []).append(va + i)
        self.callsto = callsto

    def _fset(self):
        if not hasattr(self, '_fs'):
            self._fs = set(self.fstart)
        return self._fs

    def func(self, rva):
        k = bisect.bisect_right(self.fstart, rva) - 1
        if k < 0 or rva >= self.fend[k]:
            return None
        # walk back over chained unwind entries: take the earliest contiguous start
        return self.fstart[k], self.fend[k]

    def find_string(self, s):
        out = []
        needle = s.encode() + b'\0'
        for name in ('.rdata', '.data'):
            sec = self.secs[name]; dd = sec.get_data()
            for m in re.finditer(re.escape(needle), dd):
                if m.start() == 0 or dd[m.start() - 1] == 0:
                    out.append(sec.VirtualAddress + m.start())
        return out

    def dis(self, rva, n=40):
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        off = rva - self.tva
        out = []
        for ins in md.disasm(self.tdata[off:off + n * 15], rva):
            out.append(ins)
            if len(out) >= n:
                break
        return out

    def calls_in(self, rva):
        f = self.func(rva)
        if not f:
            return []
        s, e = f
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        res = []
        for ins in md.disasm(self.tdata[s - self.tva:e - self.tva], s):
            if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
                res.append((ins.address, int(ins.op_str, 16)))
        return res

    def strs_in(self, rva, minlen=4):
        """strings referenced (lea) inside function containing rva."""
        f = self.func(rva)
        if not f:
            return []
        s, e = f
        rd = self.secs['.rdata']; rdd = rd.get_data(); rva0 = rd.VirtualAddress
        d, va = self.tdata, self.tva
        res = []
        for m in PAT.finditer(d, s - va, e - va):
            i = m.start()
            t = va + i + 7 + struct.unpack_from('<i', d, i + 3)[0]
            if rva0 <= t < rva0 + len(rdd):
                o = t - rva0
                j = rdd.find(b'\0', o, o + 300)
                if j - o >= minlen:
                    try:
                        sv = rdd[o:j].decode('ascii')
                        if sv.isprintable():
                            res.append((va + i, t, sv))
                    except UnicodeDecodeError:
                        pass
        return res


def main():
    exe, cmd, *args = sys.argv[1:]
    im = Img(exe)
    if cmd == 'str':
        for s in args:
            for t in im.find_string(s):
                for x in im.xref.get(t, []):
                    f = im.func(x)
                    print(f"{s!r} @{t:08x} xref {x:08x} func {f[0]:08x}-{f[1]:08x}" if f else f"{s!r} @{t:08x} xref {x:08x}")
    elif cmd == 'rva':
        for h in args:
            t = int(h, 16)
            for x in im.xref.get(t, []):
                f = im.func(x)
                print(f"{t:08x} xref {x:08x} func {f[0]:08x}" if f else f"{t:08x} xref {x:08x}")
    elif cmd == 'dis':
        n = int(args[1]) if len(args) > 1 else 40
        for ins in im.dis(int(args[0], 16), n):
            print(f"{ins.address:08x}: {ins.mnemonic} {ins.op_str}")
    elif cmd == 'fn':
        f = im.func(int(args[0], 16)); print(f"{f[0]:08x}-{f[1]:08x} size {f[1]-f[0]}" if f else None)
    elif cmd == 'calls':
        for a, t in im.calls_in(int(args[0], 16)):
            print(f"{a:08x} -> {t:08x}")
    elif cmd == 'callers':
        for h in args:
            t = int(h, 16); cs = im.callsto.get(t, [])
            print(f"{t:08x}: {len(cs)} callers")
            for c in cs[:30]:
                f = im.func(c); print(f"  {c:08x} in {f[0]:08x}" if f else f"  {c:08x}")
    elif cmd == 'strs':
        for a, t, s in im.strs_in(int(args[0], 16)):
            print(f"{a:08x} -> {t:08x} {s[:120]}")


if __name__ == '__main__':
    main()
