"""Shared helpers for the data-structure hot-spot study (docs/re/datastructures.md).

Loads a devtools sampling profile + call-edge csv, disassembles functions of victoria3.exe with
capstone, resolves call targets (imports by name, known engine helpers by name), and lists the
hottest callers of a function from the edge file.

Usage (ad hoc):
  re_ds_lib.py dis RVA [--max 400]        disassemble the function containing RVA
  re_ds_lib.py up  RVA [--n 8]            hottest callers (edge samples)
  re_ds_lib.py down RVA [--n 8]           hottest callees
Env: V3X_GAME_EXE (default ../../binaries/victoria3.exe), V3X_DS_PROFILE / V3X_DS_EDGES.
Requires pefile + capstone; run without python -I (user site-packages).
"""
import argparse, csv, glob, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symbolize
import capstone

EXE = symbolize.DEFAULT_EXE
_DEF_SESS = sorted(glob.glob('D:/v3x_tmp/sessions/*/profile_edges_Game_to_Empty_2.csv'))
PROFILE = os.environ.get('V3X_DS_PROFILE') or (_DEF_SESS[0].replace('profile_edges_', 'profile_') if _DEF_SESS else '')
EDGES = os.environ.get('V3X_DS_EDGES') or (_DEF_SESS[0] if _DEF_SESS else '')

_img = None
_imports = None


def img():
    global _img
    if _img is None:
        _img = symbolize.Image(EXE)
    return _img


def imports():
    """IAT slot rva -> 'dll!name'"""
    global _imports
    if _imports is None:
        _imports = {}
        pe = img().pe
        pe.parse_data_directories(directories=[pefile_dir('IMAGE_DIRECTORY_ENTRY_IMPORT')])
        for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for i in d.imports:
                nm = i.name.decode() if i.name else f'#{i.ordinal}'
                _imports[i.address - pe.OPTIONAL_HEADER.ImageBase] = d.dll.decode() + '!' + nm
    return _imports


def pefile_dir(n):
    import pefile
    return pefile.DIRECTORY_ENTRY[n]


def load_profile(path=None):
    fn = {}
    for r in csv.DictReader(open(path or PROFILE)):
        if r['module'] == 'victoria3.exe':
            fn[int(r['rva'], 16)] = (int(r['samples']), int(r['inclusive']))
    return fn


def load_edges(path=None):
    down, up = {}, {}
    for r in csv.DictReader(open(path or EDGES)):
        if r['caller_rva'].startswith('#'):
            continue
        a, b, s = int(r['caller_rva'], 16), int(r['callee_rva'], 16), int(r['samples'])
        ca, cb = r['caller_module'], r['callee_module']
        if cb == 'victoria3.exe':
            up.setdefault(b, []).append((s, a if ca == 'victoria3.exe' else (ca, a)))
        if ca == 'victoria3.exe':
            down.setdefault(a, []).append((s, b if cb == 'victoria3.exe' else (cb, b)))
    for g in (up, down):
        for k in g:
            g[k].sort(key=lambda x: -x[0])
    return down, up


def func(rva):
    return img().function(rva)


def disasm(rva, maxins=400):
    f = func(rva)
    if f:
        b, e = f
    else:
        b, e = rva, rva + 512
    code = img().read(b, e - b)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    out = []
    imp = imports()
    for ins in md.disasm(code, b):
        s = f'{ins.address:8x}: {ins.mnemonic} {ins.op_str}'
        if 'rip +' in ins.op_str or 'rip -' in ins.op_str:
            try:
                disp = int(ins.op_str.split('rip ')[1].split(']')[0].replace(' ', ''), 16)
                tgt = ins.address + ins.size + disp
                note = imp.get(tgt)
                if note:
                    s += f'    ; {note}'
                else:
                    cs = img().cstring(tgt)
                    if cs and symbolize.classify(cs):
                        s += f'    ; "{cs.decode(errors="replace")[:60]}"'
                    else:
                        s += f'    ; ={tgt:x}'
            except Exception:
                pass
        out.append(s)
        if len(out) >= maxins:
            break
    return (b, e), out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=('dis', 'up', 'down'))
    ap.add_argument('rva', nargs='+')
    ap.add_argument('--max', type=int, default=400)
    ap.add_argument('--n', type=int, default=8)
    a = ap.parse_args()
    if a.cmd == 'dis':
        for r in a.rva:
            (b, e), lines = disasm(int(r, 16), a.max)
            print(f'== {b:x}..{e:x} size {e-b}')
            print('\n'.join(lines))
        return
    fn = load_profile()
    down, up = load_edges()
    g = up if a.cmd == 'up' else down
    for r in a.rva:
        x = int(r, 16)
        print(f'== {x:x} self={fn.get(x,(0,0))[0]} incl={fn.get(x,(0,0))[1]}')
        for s, c in g.get(x, [])[:a.n]:
            if isinstance(c, tuple):
                print(f'  {s:9d}  {c[0]}+{c[1]:x}')
            else:
                f = func(c)
                h = symbolize.hints(img(), f[0], f[1], 1) if f else []
                print(f'  {s:9d}  {c:x} self={fn.get(c,(0,0))[0]} {" ".join(h)[:90]}')


def raw(rva, n=0x80):
    """Disassemble raw bytes [rva-? .. rva+n) starting right after the previous int3/ret padding."""
    data = img().read(rva - 0x40, 0x40 + n)
    # start after the last 0xcc run before rva
    st = 0
    for i in range(0x3f, 0, -1):
        if data[i] == 0xcc and data[i - 1] == 0xcc:
            st = i + 1
            break
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    return [f'{i.address:8x}: {i.mnemonic} {i.op_str}' for i in md.disasm(data[st:], rva - 0x40 + st)]


_textarr = None


def refs(lo, hi=None):
    """All instructions in .text whose RIP-relative operand points into [lo, hi). Returns
    [(insn_rva, target, text)] decoded with capstone (candidates filtered by real decoding)."""
    import numpy as np
    global _textarr
    hi = hi or lo + 1
    i_ = img()
    tva, tend = i_.text[0]
    if _textarr is None:
        _textarr = np.frombuffer(i_.read(tva, tend - tva), dtype=np.uint8)
    a = _textarr
    n = len(a) - 4
    d = (a[:n].astype(np.int64) | (a[1:n+1].astype(np.int64) << 8) | (a[2:n+2].astype(np.int64) << 16)
         | (a[3:n+3].astype(np.int64) << 24))
    d = np.where(d >= 1 << 31, d - (1 << 32), d)
    pos = np.arange(n, dtype=np.int64) + tva
    out = []
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for k in range(0, 6):          # bytes of immediate after the disp32
        t = pos + 4 + k + d
        hit = np.nonzero((t >= lo) & (t < hi))[0]
        for h in hit:
            dpos = int(pos[h])
            for st in range(dpos - 6, dpos - 1):
                code = i_.read(st, 16)
                ins = next(md.disasm(code, st), None)
                if ins and 'rip' in ins.op_str and st + ins.size == dpos + 4 + k:
                    out.append((st, int(t[h]), f'{ins.mnemonic} {ins.op_str}'))
                    break
    return sorted(set(out))


if __name__ == '__main__':
    main()

