"""Attribute devtools profiler samples to the pdxmesh per-item load (03adcfd0) and its callees.
Usage: python re_load_attrib.py <profile_latest.csv> [--module victoria3.exe] [--top 25]

profile_latest.csv (plugins/devtools write_csv): rva,samples,inclusive,module, keyed by FUNCTION START,
cumulative over the whole session. self = samples with RIP in the function, inclusive = samples
where the function was anywhere on the unwound stack (up to unwind_depth frames, counted once each).

Caveats (also printed):
- No call edges in the CSV, so a callee's inclusive count also includes calls from outside the mesh
  item (e.g. a material helper used by the GUI). Callees can also nest inside each other, so the shares
  can sum to more than 100%.
- Stacks deeper than unwind_depth never reach the root, so they are lost from the root and the callees.
  Run with unwind_depth=32 (the devtools maximum).
- The CSV is cumulative: copy it right after the load (Empty->Game) to keep in-game frames out."""
import argparse, csv, sys

ROOT = (0x03adcfd0, 'pdxmesh type load (vtable slot +0x30)')
CALLEES = [  # direct callees of 03adcfd0, 1.15.1; names are hints from the strings they reference
    (0x03dffd50, 'file lookup'),
    (0x03adbb60, 'mesh data / UV checks'),
    (0x03ad4ac0, 'meshsettings sync'),
    (0x03adc630, 'material create (pdxmesh.shader)'),
    (0x03adc9d0, 'LOD setup'),
    (0x03ad19f0, 'skeleton'),
    (0x03b2a9d0, 'animations'),
    (0x03ad5d70, 'locators'),
    (0x03ae19d0, 'anim/blendshape hashes'),
]
CONTEXT = [  # other load-phase anchors (fast_load hooks) for orientation
    (0x03ae4d80, 'mesh type loader (IterateTypesThreaded)'),
    (0x0377b5f0, 'checksum build'),
    (0x006aff80, 'game init sequence'),
    (0x006b8900, 'database registration table'),
]


def load(path, module):
    rows = {}
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            if r['module'].lower() != module.lower(): continue
            rva = int(r['rva'], 16)
            s, i = int(r['samples']), int(r['inclusive'])
            a, b = rows.get(rva, (0, 0))
            rows[rva] = (a + s, b + i)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv'); ap.add_argument('--module', default='victoria3.exe'); ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    rows = load(a.csv, a.module)
    if not rows: sys.exit(f'no rows for module {a.module}')
    total_self = sum(s for s, _ in rows.values())
    rs, ri = rows.get(ROOT[0], (0, 0))
    print(f'{a.module}: {len(rows)} functions, {total_self} self samples')
    print(f'root {ROOT[0]:08x} {ROOT[1]}: inclusive {ri} samples ({ri * 100 / max(total_self, 1):.1f}% of all exe samples), self {rs}')
    print(f'\n  {"rva":8s}  {"callee":36s} {"incl":>7s} {"% of root":>9s} {"self":>6s}')
    acc = 0
    for rva, name in sorted(CALLEES, key=lambda c: -rows.get(c[0], (0, 0))[1]):
        s, i = rows.get(rva, (0, 0)); acc += i
        print(f'  {rva:08x}  {name:36s} {i:7d} {i * 100 / max(ri, 1):8.1f}% {s:6d}')
    print(f'  {"":8s}  {"(root self + unprofiled callees, approx)":36s} {max(ri - acc, 0):7d} {max(ri - acc, 0) * 100 / max(ri, 1):8.1f}%')
    print('\n  context anchors:')
    for rva, name in CONTEXT:
        s, i = rows.get(rva, (0, 0))
        print(f'  {rva:08x}  {name:36s} incl {i:7d}  self {s:6d}')
    print(f'\n  top {a.top} exe functions by inclusive samples (map with tools/symbolize.py):')
    for rva, (s, i) in sorted(rows.items(), key=lambda kv: -kv[1][1])[:a.top]:
        print(f'  {rva:08x}  incl {i:7d}  self {s:6d}')
    print('\nCaveats: shares include calls from outside the mesh item and nested callees (can sum >100%);\n'
          'stacks deeper than unwind_depth are lost (use unwind_depth=32); the CSV is cumulative for the session.')


if __name__ == '__main__':
    main()
