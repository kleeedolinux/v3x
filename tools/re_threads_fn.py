"""Disassemble the whole function containing an RVA (bounds from .pdata), annotating RIP-relative
operands with strings / import names. Usage: re_threads_fn.py <exe> RVA [RVA...] [n=maxlines]"""
import sys, bisect, pefile, capstone

exe = sys.argv[1]
args = sys.argv[2:]
maxl = 400
if args and args[-1].startswith('n='): maxl = int(args.pop()[2:])
pe = pefile.PE(exe, fast_load=True)
pe.parse_data_directories(directories=[1, 3])
IB = pe.OPTIONAL_HEADER.ImageBase
iat = {i.address - IB: i.name.decode() for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports if i.name}
img = pe.get_memory_mapped_image()
funcs = sorted((r.struct.BeginAddress, r.struct.EndAddress) for r in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
def one(rva):
  k = bisect.bisect_right(starts, rva) - 1
  b, e = funcs[k]
  if not (b <= rva < e):
      b, e = rva, rva + 0x200
  print(f"; function {b:08x}-{e:08x} (size {e-b:#x})")

  def cstr(t):
      s = img[t:t + 120]
      z = s.find(b'\0')
      if z <= 3: return None
      s = s[:z]
      if all(32 <= c < 127 or c in (9, 10) for c in s): return s.decode()
      return None

  md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
  md.detail = True
  n = 0
  for ins in md.disasm(bytes(img[b:e]), b):
      note = ''
      for op in ins.operands:
          if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
              t = ins.address + ins.size + op.mem.disp
              if t in iat: note = f' ; [{iat[t]}]'
              else:
                  s = cstr(t)
                  note = f' ; ->{t:08x}' + (f' "{s[:90]}"' if s else '')
      if ins.mnemonic in ('call', 'jmp') and ins.operands and ins.operands[0].type == capstone.x86.X86_OP_IMM:
          t = ins.operands[0].imm
          if not (b <= t < e): note = f' ; sub_{t:08x}'
      print(f"{ins.address:08x}  {ins.mnemonic:6s} {ins.op_str}{note}")
      n += 1
      if n >= maxl: print('...'); break

for a in args: one(int(a,16))
