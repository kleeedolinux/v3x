"""Extract the static tick-task registry of victoria3.exe.
Each CTickTask_* is a global object built in a static initializer that calls the base ctor
(found as the most-called function that receives `lea rdx, "<TaskName>"`), then stores the
vtable at +0, rate at +0x28 (0 TICK,1 DAY,2 WEEK,3 MONTH,4 QUARTER,5 YEAR; ctor default 0) and
priority at +0x2c (0 EARLY,1 DEFAULT,2 LATE; ctor default 1). Strings copied into the
initializer after the ctor are dependency task names (heuristic).
Usage: re_sim_ticktasks.py <exe> <base_ctor_rva_hex> [--md]"""
import re, struct, sys
import capstone
from re_sim_lib import Img

RATE = ['TICK', 'DAY', 'WEEK', 'MONTH', 'QUARTER', 'YEAR']
PRIO = ['EARLY', 'DEFAULT', 'LATE']


def cstr_at(im, rva):
    rd = im.secs['.rdata']; d = rd.get_data(); o = rva - rd.VirtualAddress
    if not (0 < o < len(d)) or d[o - 1] != 0:
        return None
    j = d.find(b'\0', o, o + 200)
    s = d[o:j]
    if len(s) >= 3 and re.fullmatch(rb'[A-Za-z0-9_]+', s):
        return s.decode()
    return None


def vt_class(im, t):
    try:
        col = struct.unpack('<Q', im.pe.get_data(t - 8, 8))[0] - im.base
        if struct.unpack('<I', im.pe.get_data(col, 4))[0] != 1:
            return None
        td = struct.unpack('<I', im.pe.get_data(col + 12, 4))[0]
        return im.pe.get_data(td + 16, 200).split(bytes(1))[0].decode('ascii', 'replace')
    except Exception:
        return None


def analyze_helper(im, md, h):
    f = im.func(h)
    if not f:
        return None, None, []
    s, e = f
    rate = prio = None; deps = []
    for i in md.disasm(im.tdata[s - im.tva:e - im.tva], s):
        ops = i.op_str
        m = re.match(r'dword ptr \[r(?:di|cx|bx|si)\], (0x[0-9a-f]+|\d+)$', ops)
        if i.mnemonic == 'mov' and m: rate = int(m.group(1), 0)
        m = re.match(r'dword ptr \[r(?:di|cx|bx|si) \+ 4\], (0x[0-9a-f]+|\d+)$', ops)
        if i.mnemonic == 'mov' and m: prio = int(m.group(1), 0)
        for op in i.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                c = cstr_at(im, i.address + i.size + op.mem.disp)
                if c and c not in deps: deps.append(c)
    return rate, prio, deps


def main():
    exe, ctor = sys.argv[1], int(sys.argv[2], 16)
    md_out = '--md' in sys.argv
    im = Img(exe)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    rows = []
    for site in im.callsto.get(ctor, []):
        f = im.func(site)
        if not f:
            continue
        s, e = f
        ins = list(md.disasm(im.tdata[s - im.tva:e - im.tva], s))
        name = obj = None; rate, prio = 0, 1; vt = None; deps = []
        stack = {}; regs = {}; leas = {}; lastcall = None; helper = None
        for k, i in enumerate(ins):
            ops = i.op_str
            ripm = None
            for op in i.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    ripm = i.address + i.size + op.mem.disp
            if i.address < site:
                if i.mnemonic == 'lea' and ops.startswith('rdx') and ripm:
                    name = cstr_at(im, ripm)
                if i.mnemonic == 'lea' and ripm:
                    leas[ops.split(',')[0]] = ripm
                    if ops.startswith('rcx'):
                        obj = ripm
                m2 = re.match(r'rcx, (r\w+)$', ops)
                if i.mnemonic == 'mov' and m2 and m2.group(1) in leas:
                    obj = leas[m2.group(1)]
                continue
            if obj is None:
                break
            if i.mnemonic == 'call' and ops.startswith('0x'):
                lastcall = int(ops, 16)
            if i.mnemonic == 'mov' and re.match(r'e?[a-d]x, dword ptr \[rax\]$|ecx, dword ptr \[rax\]$', ops) and lastcall and helper is None:
                helper = lastcall
                hr, hp, hd = analyze_helper(im, md, helper)
                rate = hr if hr is not None else rate
                prio = hp if hp is not None else prio
                deps += [d for d in hd if d not in deps and d != name]
            m = re.match(r'dword ptr \[(r[sb]p [+-] 0x[0-9a-f]+)\], (0x[0-9a-f]+|\d+)$', ops)
            if i.mnemonic == 'mov' and m:
                stack[m.group(1)] = int(m.group(2), 0)
            m = re.match(r'(e[a-d]x|r\d+d), dword ptr \[(r[sb]p [+-] 0x[0-9a-f]+)\]$', ops)
            if i.mnemonic == 'mov' and m:
                regs[m.group(1)] = stack.get(m.group(2))
            if i.mnemonic == 'mov' and ripm and ops.startswith('dword ptr [rip'):
                src = ops.split(', ')[-1]
                v = regs.get(src) if src in regs else (int(src, 0) if re.fullmatch(r'0x[0-9a-f]+|\d+', src) else None)
                off = ripm - obj
                if off == 0x28 and v is not None: rate = v
                if off == 0x2c and v is not None: prio = v
            if i.mnemonic == 'lea' and ripm:
                leas[ops.split(',')[0]] = ripm
            if i.mnemonic == 'mov' and ripm and ripm == obj and ops.startswith('qword ptr [rip') and vt is None:
                vt = leas.get(ops.split(', ')[-1])
            if ripm and i.mnemonic in ('vmovups', 'vmovdqu', 'movups', 'movzx', 'mov', 'movsd', 'movq', 'vmovq', 'lea'):
                c = cstr_at(im, ripm)
                if c and c != name and c not in deps:
                    deps.append(c)
        if name:
            fn7 = None
            if vt:  # vt = CJominiTickTaskRegistryHelper<T> vtable; slot 1 = Create() -> new T, sets T's vtable
                create = struct.unpack('<Q', im.pe.get_data(vt + 8, 8))[0] - im.base
                vt = None
                for ci in im.dis(create, 60):
                    if ci.mnemonic == 'lea' and '[rip + ' in ci.op_str:
                        t = ci.address + ci.size + int(ci.op_str.split('+ ')[1].rstrip(']'), 16)
                        cn = vt_class(im, t)
                        if cn and 'CTickTask' in cn:
                            vt = t
                            fn7 = struct.unpack('<Q', im.pe.get_data(vt + 8 * 7, 8))[0] - im.base
                            break
            rows.append((name, rate, prio, deps, s, vt, fn7))
    rows.sort(key=lambda r: (r[1], r[0]))
    if md_out:
        print('| task | rate | prio | deps (heuristic) | vtable | slot7 fn |')
        print('|---|---|---|---|---|---|')
    for name, rate, prio, deps, s, vt, fn7 in rows:
        r = RATE[rate] if 0 <= rate < len(RATE) else rate
        p = PRIO[prio] if 0 <= prio < len(PRIO) else prio
        if md_out:
            print(f"| {name} | {r} | {p} | {', '.join(deps[:6])} | {vt or 0:08x} | {fn7 or 0:08x} |")
        else:
            print(f"{name:55s} {r:8s} {p:8s} init={s:08x} vt={vt or 0:08x} fn7={fn7 or 0:08x} deps={','.join(deps)}")
    print(f"# {len(rows)} tasks", file=sys.stderr)


if __name__ == '__main__':
    main()
