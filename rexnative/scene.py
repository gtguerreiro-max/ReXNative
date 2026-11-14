"""The scene surface of a game, from one frame's resolves (the native renderer's resolve dump:
--pgr4_native_resolve_dump=<dir>, then create <dir>/dump.request; it writes
f<frame>-resolves.jsonl).

The upscaler (DLSS / DLAA) and DLSS 5 recognise the scene by RB_SURFACE_INFO (pitch and MSAA,
upscaler.h kSceneSurface / kSceneSurfaceMask) and its size. The scene is taken to be the
colour resolve that covers (nearly) the whole frame, preferring MSAA and an HDR format, and the
earliest of those in the frame (post-processing and the UI come after it).
"""
import json

COLOR_FORMATS = {0: 'k_8_8_8_8', 1: 'k_8_8_8_8_GAMMA', 2: 'k_2_10_10_10',
                 3: 'k_2_10_10_10_FLOAT (7e3)', 4: 'k_16_16', 5: 'k_16_16_16_16',
                 6: 'k_16_16_FLOAT', 7: 'k_16_16_16_16_FLOAT', 10: 'k_2_10_10_10_AS_10_10_10_10',
                 12: 'k_2_10_10_10_FLOAT_AS_16_16_16_16', 14: 'k_32_FLOAT', 15: 'k_32_32_FLOAT'}
HDR = {3, 5, 6, 7, 12, 14, 15}
MSAA = {0: '1x', 1: '2x', 2: '4x'}


def read(path):
    out = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.strip():
                out.append(json.loads(line))
    return out


def find(resolves):
    colour = [r for r in resolves if r.get('source', 0) < 4]
    if not colour:
        return None
    def area(r):
        x0, y0, x1, y1 = r['rect']
        return max(0, x1 - x0) * max(0, y1 - y0)
    full = max(area(r) for r in colour)
    big = [r for r in colour if area(r) >= 0.9 * full]
    def key(r):
        msaa = (r['surface_info'] >> 16) & 3
        fmt = (r['source_info'] >> 16) & 0xF
        return (msaa > 0, fmt in HDR, -r['index'])
    best = max(big, key=key)
    si = best['surface_info']
    fmt = (best['source_info'] >> 16) & 0xF
    x0, y0, x1, y1 = best['rect']
    return {
        'index': best['index'],
        'kSceneSurfaceMask': 0x33FFF,
        'kSceneSurface': si & 0x33FFF,
        'pitch': si & 0x3FFF,
        'msaa': MSAA.get((si >> 16) & 3, '?'),
        'width': x1 - x0,
        'height': best.get('dest_height', y1 - y0),
        'format': COLOR_FORMATS.get(fmt, str(fmt)),
        'hdr': fmt in HDR,
        'later_full_frame': [r['index'] for r in big if r['index'] > best['index']],
    }


def describe(s):
    lines = [
        'Cena: resolve #%d, %dx%d, pitch %d, MSAA %s, formato %s%s.' % (
            s['index'], s['width'], s['height'], s['pitch'], s['msaa'], s['format'],
            ' (HDR)' if s['hdr'] else ''),
        'overrides.json: {"scene": {"kSceneSurfaceMask": %d, "kSceneSurface": %d, "width": %d, '
        '"scaled_pitch": %d, "height": %d}}' % (s['kSceneSurfaceMask'], s['kSceneSurface'],
                                                s['width'], s['pitch'], s['height']),
    ]
    if s['hdr']:
        lines.append('DLSS 5: o resolve da cena é HDR, antes do tone mapping: '
                     'pgr4_native_dlss5_stage = "scene".')
    else:
        lines.append('DLSS 5: a cena não está num formato HDR neste resolve (tone mapping já '
                     'feito ou cena LDR): confirmar com o RenderDoc qual o passe HDR, ou usar '
                     'pgr4_native_dlss5_stage = "front".')
    if s['later_full_frame']:
        lines.append('Resolves de ecrã inteiro depois da cena (pós-processamento / UI): %s.'
                     % ', '.join('#%d' % i for i in s['later_full_frame']))
    return '\n'.join(lines)
