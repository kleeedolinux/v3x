"""Name hot functions in a symbol-less exe by the strings they reference.

For each RVA: find the enclosing function via .pdata (following chained unwind info to the
primary entry), then scan its body for RIP-relative `lea reg,[rip+x]` that point at C strings in
.rdata/.data. Source paths (*.cpp / *.h) rank first, then identifier-like strings, then text.
Functions with no strings of their own get hints from up to 8 direct callees ("via callee").

Usage:
  symbolize.py [--exe victoria3.exe] [--top 50] [--hints 3] (profile_latest.csv | RVA ...)
  RVAs are hex (0x1234 or 1234). CSV = devtools' v3x/profile_latest.csv (rva,samples,inclusive,module);
  rows from other modules are skipped.

Requires pefile (pip install pefile). Run it WITHOUT python -I: isolated mode skips the user
site-packages where pefile is usually installed ("No module named 'pefile'").
"""
import argparse, bisect, csv, os, re, struct, sys
import pefile

DEFAULT_EXE = r"D:\SteamLibrary\steamapps\common\Victoria 3\binaries\victoria3.exe"
LEA = re.compile(rb'[\x40-\x4f]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
CALL = re.compile(rb'\xe8', re.S)
UNW_FLAG_CHAININFO = 4


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.data = self.pe.__data__
        self.secs = [(s.Name.rstrip(b'\0').decode(errors='replace'), s.VirtualAddress,
                      max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData, s.SizeOfRawData)
                     for s in self.pe.sections]
        self.text = [(va, va + vs) for n, va, vs, _, _ in self.secs if n.startswith('.text')]
        self.strsec = [(va, va + vs) for n, va, vs, _, _ in self.secs if n in ('.rdata', '.data')]
        d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']]
        raw = self.read(d.VirtualAddress, d.Size)
        self.pdata = list(struct.iter_unpack('<III', raw[:len(raw) // 12 * 12]))
        self.begins = [b for b, _, _ in self.pdata]

    def off(self, rva):
        for _, va, vs, ptr, rs in self.secs:
            if va <= rva < va + vs:
                return ptr + (rva - va) if rva - va < rs else None
        return None

    def read(self, rva, n):
        o = self.off(rva)
        return bytes(self.data[o:o + n]) if o is not None else b''

    def in_str(self, rva):
        return any(lo <= rva < hi for lo, hi in self.strsec)

    def in_text(self, rva):
        return any(lo <= rva < hi for lo, hi in self.text)

    def entry(self, rva):
        i = bisect.bisect_right(self.begins, rva) - 1
        if i < 0:
            return None
        b, e, u = self.pdata[i]
        return (b, e, u) if b <= rva < e else None

    def primary(self, ent):
        b, e, u = ent
        for _ in range(32):
            if u & 1:                      # indirect: points at another RUNTIME_FUNCTION
                b, e, u = struct.unpack('<III', self.read(u & ~1, 12))
                continue
            hdr = self.read(u, 4)
            if len(hdr) < 4 or not ((hdr[0] >> 3) & UNW_FLAG_CHAININFO):
                break
            codes = hdr[2]
            b, e, u = struct.unpack('<III', self.read(u + 4 + ((codes + 1) & ~1) * 2, 12))
        return b, e

    def function(self, rva):
        ent = self.entry(rva)
        return self.primary(ent) if ent else None

    def cstring(self, rva, maxlen=240):
        raw = self.read(rva, maxlen + 1)
        z = raw.find(b'\0')
        return raw[:z] if z >= 0 else None


def classify(s):
    """3 = source path, 2 = identifier-like, 1 = other text, 0 = not a string"""
    if s is None or len(s) < 4 or any(c < 0x20 or c > 0x7e for c in s) or not re.search(rb'[A-Za-z]', s):
        return 0
    if re.search(rb'\.(cpp|h|hpp|c)(:\d+)?$', s):
        return 3
    if re.fullmatch(rb'[A-Za-z0-9_:.<>\-]+', s):
        return 2
    return 1 if len(s) >= 6 else 0


def short_path(s):
    parts = re.split(r'[\\/]', s)
    return '\\'.join(parts[-3:]) if len(parts) > 3 else s


def strings_in(img, begin, end):
    body = img.read(begin, min(end - begin, 65536))
    seen, out = set(), []
    for m in LEA.finditer(body):
        i = m.start()
        if i + 7 > len(body):
            break
        tgt = begin + i + 7 + struct.unpack_from('<i', body, i + 3)[0]
        if tgt in seen or not img.in_str(tgt):
            continue
        seen.add(tgt)
        s = img.cstring(tgt)
        c = classify(s)
        if c:
            out.append((c, i, s.decode()))
    out.sort(key=lambda x: (-x[0], x[1]))
    return out


def callees(img, begin, end, limit=8):
    body = img.read(begin, min(end - begin, 65536))
    res = []
    for m in CALL.finditer(body):
        i = m.start()
        if i + 5 > len(body):
            break
        tgt = begin + i + 5 + struct.unpack_from('<i', body, i + 1)[0]
        if img.in_text(tgt) and tgt not in res and img.entry(tgt) and img.entry(tgt)[0] == tgt:
            res.append(tgt)
            if len(res) >= limit:
                break
    return res


def hints(img, begin, end, n):
    ss = strings_in(img, begin, end)
    if ss:
        return ['"%s"' % (short_path(s) if c == 3 else s[:70]) for c, _, s in ss[:n]]
    out = []
    for cal in callees(img, begin, end):
        f = img.function(cal)
        if not f:
            continue
        cs = strings_in(img, *f)
        if cs:
            c, _, s = cs[0]
            out.append('via %x: "%s"' % (cal, short_path(s) if c == 3 else s[:60]))
        if len(out) >= n:
            break
    return out


def read_inputs(args, exe_name):
    rows = []
    for a in args.inputs:
        if os.path.isfile(a):
            with open(a, newline='') as f:
                for r in csv.DictReader(f):
                    mod = (r.get('module') or exe_name).lower()
                    if mod != exe_name.lower():
                        continue
                    rows.append((int(r['rva'], 16), int(r.get('samples') or 0), int(r.get('inclusive') or 0)))
        else:
            rows.append((int(a, 16), 0, 0))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', help='profile_latest.csv and/or hex RVAs')
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--top', type=int, default=50)
    ap.add_argument('--hints', type=int, default=3)
    ap.add_argument('--by', choices=('self', 'incl'), default='self')
    args = ap.parse_args()
    img = Image(args.exe)
    rows = read_inputs(args, os.path.basename(args.exe))
    total = sum(r[1] for r in rows) or 1
    if args.by == 'incl':
        rows.sort(key=lambda r: -r[2])
    print(f"{os.path.basename(args.exe)}: {len(img.pdata)} .pdata entries; {len(rows)} input rows, {total} self samples")
    print(f"{'rva':>10} {'func':>10} {'size':>7} {'self':>7} {'self%':>6} {'incl':>7}  hints")
    for rva, s, inc in rows[:args.top]:
        f = img.function(rva)
        if not f:
            print(f"{rva:10x} {'-':>10} {'-':>7} {s:7d} {s * 100 / total:6.2f} {inc:7d}  (no .pdata: leaf or not code)")
            continue
        b, e = f
        h = hints(img, b, e, args.hints)
        print(f"{rva:10x} {b:10x} {e - b:7d} {s:7d} {s * 100 / total:6.2f} {inc:7d}  {' '.join(h)}")


if __name__ == '__main__':
    sys.exit(main())
