"""keep-history RE helper: re_sim_lib with its index cache on D: (C: is nearly full).
Usage: re_hist_lib.py <exe> <re_sim_lib command> ARGS...
       re_hist_lib.py <exe> around HEX BEFORE AFTER   (disassemble around any RVA, resynced)
       re_hist_lib.py <exe> func HEX                  (disassemble whole .pdata function)"""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re_sim_lib, pexref
re_sim_lib.CACHE = os.environ.get('RE_HIST_CACHE', os.path.join(tempfile.gettempdir(), 'v3x_re', 're_hist_index.pkl'))
if __name__ == '__main__':
    exe, cmd = sys.argv[1], sys.argv[2]
    if cmd == 'around':
        pe, text, data = pexref.load(exe)
        pexref.dis(pe, text, data, int(sys.argv[3], 16), int(sys.argv[4]), int(sys.argv[5]))
    elif cmd == 'func':
        im = re_sim_lib.Img(exe)
        s, e = im.func(int(sys.argv[3], 16))
        pe, text, data = pexref.load(exe)
        pexref.dis(pe, text, data, s, 0, e - s)
    else:
        re_sim_lib.main()
