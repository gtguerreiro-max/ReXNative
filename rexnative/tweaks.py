"""Per-game settings the renderer needs to look like the console where an emulated detail still
differs, written into the game's settings by `project` (like dlss5.py's look; a value already
there stays, so a player can change or empty it).

KNOWN: title id -> [(key in [rexnative], value as written in the toml)].
"""
import os
import re

KNOWN = {
    # Forza Horizon: its motion blur keeps a car's contact shadow box sharp (stencil 0x80, in the
    # cars' class below c253.w = 0.501968), which the console's picture does not show (the ground
    # under the cars blurs like the road around them in footage of the Xbox 360). With 0.5 the
    # box blurs with the road (docs/NOTES.md, "A car's contact shadow in the motion blur").
    '4D5309C9': [('pixel_constant_override', '"E68FE55ECD482EC5:c253.w=0.5"')],
}


def apply(work, settings_files, log=print):
    """Sets KNOWN's [rexnative] keys for the prepared game in each settings file that has none."""
    import json
    from .deck import toml_set
    try:
        prep = json.load(open(os.path.join(work, 'prepare.json'), encoding='utf-8'))
    except (OSError, ValueError):
        prep = {}
    tid = ((prep.get('xex') or {}).get('title_id') or '').upper()
    tweaks = KNOWN.get(tid)
    if not tweaks:
        return []
    for path in settings_files:
        if not os.path.exists(path):
            continue
        text = open(path, encoding='utf-8').read()
        changed = False
        for key, value in tweaks:
            if re.search(r'(?m)^\s*%s\s*=' % re.escape(key), text):
                continue
            text = toml_set(text, 'rexnative', key, value)
            changed = True
        if changed:
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
    log('%s: picture tweaks %s' % (os.path.basename(os.path.normpath(work)),
                                   ', '.join('%s = %s' % kv for kv in tweaks)))
    return tweaks
