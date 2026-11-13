"""Asset inventory for load-time RE: size/count per subfolder and per extension.
Usage: re_load_assets.py <dir> [--depth N] [--top K]
Prints MB and file counts grouped by first N path components and by extension."""
import argparse, os, collections

def scan(root, depth):
    by_dir = collections.Counter(); n_dir = collections.Counter()
    by_ext = collections.Counter(); n_ext = collections.Counter()
    for dp, _, files in os.walk(root):
        rel = os.path.relpath(dp, root).replace('\\', '/')
        key = '/'.join(rel.split('/')[:depth]) if rel != '.' else '.'
        for f in files:
            try:
                sz = os.path.getsize(os.path.join(dp, f))
            except OSError:
                continue
            ext = os.path.splitext(f)[1].lower() or '(none)'
            by_dir[key] += sz; n_dir[key] += 1
            by_ext[ext] += sz; n_ext[ext] += 1
    return by_dir, n_dir, by_ext, n_ext

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('root'); ap.add_argument('--depth', type=int, default=1)
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    bd, nd, be, ne = scan(a.root, a.depth)
    tot = sum(bd.values())
    print(f"total {tot/2**20:.1f} MB, {sum(nd.values())} files")
    print("-- by folder")
    for k, v in bd.most_common(a.top):
        print(f"{v/2**20:9.1f} MB {nd[k]:7d}  {k}")
    print("-- by extension")
    for k, v in be.most_common(a.top):
        print(f"{v/2**20:9.1f} MB {ne[k]:7d}  {k}")
