"""Regenerates this mod's shader overrides from the vanilla game files.

    python mods/v3x_lowspec_shaders/make_mod.py [--game "<Steam>/steamapps/common/Victoria 3"]

Each override is a copy of the vanilla file with a few exact-text replacements (EDITS below).
If any replacement text is not found exactly as many times as expected (a game patch changed the
file), the script stops and writes nothing, so a stale mod is never produced silently. Re-run after
every game update; see README.md for what each edit does.
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GAME = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
T = '\t'

def tabs(n):
    return T * n

EDITS = {
    # 1. Terrain: the normal terrain effects use the engine's own low-spec terrain shaders.
    #    The engine picks "PdxTerrainLowSpec" only at shader_quality = low (victoria3.exe 1.15.1
    #    fn 0x2802050); this makes medium/high use them too.
    'gfx/FX/pdxterrain.shader': [
        ('Effect PdxTerrain\n{\n\tVertexShader = "VertexShader"\n\tPixelShader = "PixelShader"\n',
         'Effect PdxTerrain\n{\n\tVertexShader = "VertexShaderLowSpec"\t# V3X lowspec (was VertexShader)\n'
         '\tPixelShader = "PixelShaderLowSpec"\t# V3X lowspec (was PixelShader)\n', 1),
        ('Effect PdxTerrainSkirt\n{\n\tVertexShader = "VertexShaderSkirt"\n\tPixelShader = "PixelShader"\n',
         'Effect PdxTerrainSkirt\n{\n\tVertexShader = "VertexShaderLowSpecSkirt"\t# V3X lowspec (was VertexShaderSkirt)\n'
         '\tPixelShader = "PixelShaderLowSpec"\t# V3X lowspec (was PixelShader)\n', 1),
    ],
    # 2. Water: always the low-spec water function, no shadow sampling on water.
    'gfx/FX/pdxwater.shader': [
        (tabs(4) + '#if defined( LOW_QUALITY_SHADERS )\n' + tabs(5) + 'Water = CalcWaterLowSpec( Input );\n',
         tabs(4) + '#if 1 // V3X lowspec (was: #if defined( LOW_QUALITY_SHADERS ))\n' + tabs(5) + 'Water = CalcWaterLowSpec( Input );\n', 1),
        (tabs(4) + '// Simple shadows\n' + tabs(4) + '#ifndef LOW_QUALITY_SHADERS\n',
         tabs(4) + '// Simple shadows\n' + tabs(4) + '#if 0 // V3X lowspec (was: #ifndef LOW_QUALITY_SHADERS)\n', 1),
    ],
    # 3. Trees (and bushes): no wind sway in any of the 4 vertex shaders (main + shadow pass,
    #    standard + map object); pixel shader always takes its low-quality branches (no
    #    pollution/devastation/seasons/harvest tinting, no PBR sun + second sun, no backlight).
    'gfx/FX/tree.shader': [
        (tabs(4) + '#if !defined( GUI_SHADER )\n' + tabs(5) + '#if defined( TREE_BUSH )\n',
         tabs(4) + '#if 0 // V3X lowspec: no wind sway (was: #if !defined( GUI_SHADER ))\n' + tabs(5) + '#if defined( TREE_BUSH )\n', 4),
        (tabs(4) + '#if !defined( LOW_QUALITY_SHADERS ) && !defined( GUI_SHADER )\n' + tabs(5) + 'ApplyPollutionTrees',
         tabs(4) + '#if 0 // V3X lowspec (was: #if !defined( LOW_QUALITY_SHADERS ) && !defined( GUI_SHADER ))\n' + tabs(5) + 'ApplyPollutionTrees', 1),
        (tabs(4) + '#ifndef LOW_QUALITY_SHADERS\n' + tabs(5) + 'Diffuse.rgb = CalculateSunLighting(',
         tabs(4) + '#if 0 // V3X lowspec (was: #ifndef LOW_QUALITY_SHADERS)\n' + tabs(5) + 'Diffuse.rgb = CalculateSunLighting(', 1),
        (tabs(4) + '#if defined( BACKLIGHT ) && !defined( LOW_QUALITY_SHADERS )\n',
         tabs(4) + '#if 0 // V3X lowspec (was: #if defined( BACKLIGHT ) && !defined( LOW_QUALITY_SHADERS ))\n', 1),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=DEFAULT_GAME, help='Victoria 3 install folder')
    ap.add_argument('--check', action='store_true', help='only verify the edits still apply')
    a = ap.parse_args()
    out = {}
    for rel, edits in EDITS.items():
        src = os.path.join(a.game, 'game', rel)
        with open(src, 'r', encoding='utf-8-sig', newline='') as f:
            text = f.read()
        if '\r\n' in text:
            sys.exit(f'{rel}: CRLF line endings, edits expect LF')
        for old, new, count in edits:
            n = text.count(old)
            if n != count:
                sys.exit(f'{rel}: expected {count} match(es), found {n} for:\n{old}')
            text = text.replace(old, new)
        out[rel] = text
    if a.check:
        print('all edits apply:', ', '.join(out))
        return
    for rel, text in out.items():
        dst = os.path.join(HERE, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'w', encoding='utf-8', newline='') as f:  # vanilla files have no BOM
            f.write(text)
        print('wrote', os.path.relpath(dst, HERE))


if __name__ == '__main__':
    main()
