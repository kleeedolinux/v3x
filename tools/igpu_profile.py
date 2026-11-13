"""Apply the V3X integrated-GPU graphics recipe to Victoria 3's pdx_settings.json.

    python tools/igpu_profile.py --dry-run          show what would change, write nothing
    python tools/igpu_profile.py                    back up, then apply
    python tools/igpu_profile.py --restore FILE     copy a backup back over pdx_settings.json
    options: --settings PATH (default: Documents/Paradox Interactive/Victoria 3/pdx_settings.json)
             --fps-cap N     also set setting_framerate_cap (30/60/90/120/144/165, 0 = off)
             --no-seasons    also turn off seasons + day/night cycle (keys not verified, see below)

Run it with the game closed: the game rewrites the file on exit.
Before writing, it always copies the current file to pdx_settings.json.bak-YYYYmmdd-HHMMSS
(next to it).

Option names come from the setting registrations in victoria3.exe 1.15.1 and from
game/compound_settings.txt (see docs/re/graphics.md section 2). The keys in RECIPE are the ones
the game itself writes under "Graphics". Two caveats, not verified at runtime:
  * the "quality" preset is set to low and every value repeats the low preset except the few
    marked (*) in RECIPE. If the engine re-applies the preset at load, only those (*) values
    can be lost; you still get the low preset.
  * texture_streaming_cache is a size in MB; the slider range was not extracted, and the engine
    is expected to clamp out-of-range values.
"""
import argparse, datetime, json, os, shutil, sys
from collections import OrderedDict

DEFAULT = os.path.join(os.path.expanduser('~'), 'Documents', 'Paradox Interactive', 'Victoria 3',
                       'pdx_settings.json')

# "quality" = low applies the engine's own low preset (game/compound_settings.txt), so the result
# holds even if the engine re-applies the preset at load. The explicit values below repeat that
# preset, plus the few deliberate differences marked (*). Ordered by expected iGPU gain.
RECIPE = OrderedDict([
    ('quality', 'low'),                      # preset; medium -> low
    ('anti_aliasing', 'FXAA'),               # (*) low preset = DISABLED; FXAA is a cheap post pass, MSAAx2 is not
    ('ssao', False),                         # map SSAO pass
    ('depthoffield', False),                 # 16-sample DOF
    ('refraction_quality', 'disabled'),      # water refraction scene copy
    ('shadowmap_resolution', 'disabled'),    # 2048 on medium
    ('bloom_quality', 'low'),
    ('shader_quality', 'low'),               # LOW_QUALITY_SHADERS permutations
    ('mapobject_quality', 'low'),            # trees / map objects
    ('buildings_cutoff', True),
    ('mesh_lod_bias', 'low'),
    ('mesh_lod_fade', False),
    ('lensflare', False),                    # (*) hidden setting (not in the menu)
    ('secondary_lensflare', False),          # (*) hidden setting
    ('portraits_ssao', False),
    ('animated_portraits', False),
    ('portrait_multi_sampling', 'x2'),
    ('portrait_render_scale', 'none'),
    ('anisotropic_filtering', 'DISABLED'),
    ('texture_quality', 'low'),              # shared system RAM on Iris Xe
    ('texture_streaming', True),             # (*) every preset forces it off
    ('texture_streaming_cache', 768),        # (*) MB
])
# Gentler preset (lead's choice): keep the engine's medium preset (models, shaders, textures,
# shadows, animated portraits stay medium) and only drop the costly post effects. Only values
# already known from the low recipe / medium preset are used.
LOWMED = OrderedDict([
    ('quality', 'medium'),                   # keep the medium preset as the base
    ('anti_aliasing', 'FXAA'),               # MSAAx2 -> FXAA (cheap post pass)
    ('ssao', False),                         # map SSAO pass
    ('depthoffield', False),                 # 16-sample DOF
    ('bloom_quality', 'low'),
    ('lensflare', False),                    # hidden setting
    ('secondary_lensflare', False),          # hidden setting
    ('portraits_ssao', False),
    ('portrait_multi_sampling', 'x2'),
    ('texture_streaming', True),             # every preset forces it off
    ('texture_streaming_cache', 768),        # MB
])
PRESETS = {'low': RECIPE, 'lowmed': LOWMED}
NO_SEASONS = OrderedDict([                   # category -> {key: value}; key names not verified
    ('seasons', OrderedDict([('seasons_enabled', False), ('seasons_foliage', False),
                             ('seasons_sea_ice', False)])),
    ('daynight_cycle', OrderedDict([('daynight_cycle', False)])),
])
FPS_CAPS = {0: 'off', 30: '30', 60: '60', 90: '90', 120: '120', 144: '144', 165: '165'}


def load(path):
    with open(path, 'r', encoding='utf-8-sig') as f:
        return json.load(f, object_pairs_hook=OrderedDict)


def dump(data):
    return json.dumps(data, indent='\t', ensure_ascii=False) + '\n'


def plan(data, fps_cap, no_seasons, recipe=None):
    """Returns [(category, key, old, new)] for every value that differs."""
    changes = []
    def want(cat, key, val):
        old = data.get(cat, {}).get(key, '<unset>')
        if old != val:
            changes.append((cat, key, old, val))
    for k, v in (recipe or RECIPE).items():
        want('Graphics', k, v)
    if fps_cap is not None:
        want('Graphics', 'setting_framerate_cap', FPS_CAPS[fps_cap])
    if no_seasons:
        for cat, kv in NO_SEASONS.items():
            for k, v in kv.items():
                want(cat, k, v)
    return changes


def apply(data, changes):
    for cat, key, _old, new in changes:
        if not isinstance(data.get(cat), dict):
            data[cat] = OrderedDict()
        data[cat][key] = new
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--settings', default=DEFAULT)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--restore', metavar='BACKUP')
    ap.add_argument('--fps-cap', type=int, choices=sorted(FPS_CAPS))
    ap.add_argument('--no-seasons', action='store_true')
    ap.add_argument('--preset', choices=sorted(PRESETS), default='low', help='low (aggressive iGPU recipe) or lowmed (medium base, costly effects off)')
    a = ap.parse_args()
    path = a.settings

    if a.restore:
        if not os.path.isfile(a.restore):
            sys.exit(f'backup not found: {a.restore}')
        load(a.restore)  # refuse to restore something that is not valid JSON
        if a.dry_run:
            print(f'would copy {a.restore} -> {path}')
            return
        shutil.copy2(a.restore, path)
        print(f'restored {path} from {a.restore}')
        return

    if not os.path.isfile(path):
        sys.exit(f'settings file not found: {path}')
    data = load(path)
    changes = plan(data, a.fps_cap, a.no_seasons, PRESETS[a.preset])
    if not changes:
        print('already applied, nothing to do')
        return
    w = max(len(f'{c}.{k}') for c, k, _, _ in changes)
    for cat, key, old, new in changes:
        print(f'  {cat + "." + key:<{w}}  {json.dumps(old) if old != "<unset>" else old:>12}  ->  {json.dumps(new)}')
    print(f'{len(changes)} change(s) to {path}')
    if a.dry_run:
        print('dry run: nothing written')
        return
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = f'{path}.bak-{stamp}'
    shutil.copy2(path, backup)
    text = dump(apply(data, changes))
    tmp = path + '.v3x-tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.replace(tmp, path)
    print(f'backup: {backup}')
    print(f'written: {path}  (undo: python tools/igpu_profile.py --restore "{backup}")')


if __name__ == '__main__':
    main()
