"""console_plus RE helper: list the engine's static console command descriptors (SCommandData).

Usage: re_console_cmds.py <victoria3.exe> [--cb]   (--cb: only commands with an autocomplete callback)

Static init code writes each command's SCommandData (0x210 bytes, see docs/re/console.md) into
.data with this shape (1.15.1):
    lea rdx,"name"        ; lea rcx,[D+0x08] ; call CString::CString(const char*)
    lea rdx,"description" ; lea rcx,[D+0x90] ; call CString::CString
    lea rax,OnExecute     ; mov [D+0xb0],rax
    mov [D+0xb8],<0 | lea autocomplete> ; mov [D+0xc0],<0>   ; autocomplete callback + user
    mov dword [D+0xc8],<arg count> ; lea rdx,"arg name" ; lea rcx,[D+0xd0] ...
We collect every RIP-relative store/lea into .data, then reconstruct records around each
`mov [D+0xb0], rax <- lea .text` store."""
import re, struct, sys
import pefile

exe = sys.argv[1]
only_cb = '--cb' in sys.argv
pe = pefile.PE(exe, fast_load=True)
sec = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
tx = sec['.text']; t = tx.get_data(); tva = tx.VirtualAddress
rd = sec['.rdata']; rdd = rd.get_data(); rva_rd = rd.VirtualAddress
dt = sec['.data']; dva = dt.VirtualAddress; dend = dva + dt.Misc_VirtualSize
def in_text(r): return tva <= r < tva + len(t)
def in_data(r): return dva <= r < dend
def cstr(r):
    if not (rva_rd <= r < rva_rd + len(rdd)): return None
    o = r - rva_rd; j = rdd.find(b'\0', o, o + 300)
    if j < 0: return None
    try: s = rdd[o:j].decode('ascii')
    except UnicodeDecodeError: return None
    return s if s.isprintable() else None

# 1) lea rax,[rip+T] ; mov [rip+D],rax  (48 8d 05 .. 48 89 05 ..)
store_text = {}   # D -> T
for m in re.finditer(rb'([\x48\x4c])\x8d([\x05\x0d\x15\x1d\x25\x2d\x35\x3d])(....)([\x48\x4c])\x89([\x05\x0d\x15\x1d\x25\x2d\x35\x3d])(....)', t, re.S):
    if m.group(1) != m.group(4) or m.group(2) != m.group(5): continue   # same register
    i = m.start()
    T = tva + i + 7 + struct.unpack('<i', m.group(3))[0]
    D = tva + i + 14 + struct.unpack('<i', m.group(6))[0]
    if in_text(T) and in_data(D): store_text[D] = (T, tva + i)
# 2) lea rdx,[rip+S] ; lea rcx,[rip+D] ; call  (CString ctor from literal)
str_init = {}     # D -> string
for m in re.finditer(rb'\x48\x8d([\x0d\x15])(....)\x48\x8d([\x0d\x15])(....)\xe8', t, re.S):
    if m.group(1) == m.group(3): continue
    i = m.start()
    a = tva + i + 7 + struct.unpack('<i', m.group(2))[0]
    b = tva + i + 14 + struct.unpack('<i', m.group(4))[0]
    S, D = (a, b) if m.group(1) == b'\x15' else (b, a)
    s = cstr(S)
    if s is not None and in_data(D): str_init[D] = s
# 3) mov dword [rip+D], imm32 (c7 05 disp imm)
imm = {}
for m in re.finditer(rb'\xc7\x05(....)(....)', t, re.S):
    i = m.start()
    D = tva + i + 10 + struct.unpack('<i', m.group(1))[0]
    if in_data(D): imm[D] = struct.unpack('<i', m.group(2))[0]

n = ncb = 0
for D, (T, site) in sorted(store_text.items()):
    B = D - 0xb0
    name = str_init.get(B + 8)
    desc = str_init.get(B + 0x90)
    if name is None or desc is None: continue
    cb = store_text.get(B + 0xb8)
    n += 1
    if cb: ncb += 1
    if only_cb and not cb: continue
    args = [str_init.get(B + 0xd0 + 0x20 * k) for k in range(10)]
    args = [a for a in args if a]
    aliases = [str_init.get(B + 0x30 + 0x20 * k) for k in range(3)]
    aliases = [a for a in aliases if a]
    print(f'{B:08x} {name:<32} exec={T:08x} cb={(cb[0] if cb else 0):08x} nargs={imm.get(B + 0xc8, "?")}'
          f' args={args} alias={aliases} | {desc[:70]}')
print(f'# {n} commands, {ncb} with an autocomplete callback', file=sys.stderr)
