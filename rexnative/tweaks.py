"""Per-game settings the renderer needs to look like the console where an emulated detail still
differs, written into the game's settings by `project` (like dlss5.py's look; a value already
there stays, so a player can change or empty it).

KNOWN: title id -> [(key in [rexnative], value as written in the toml)]. None now: what a game
needed here became the renderer's own emulation.
RETIRED: what KNOWN wrote once and the renderer now does by itself; `project` takes it out of the
settings again where it is still the value written (a value the player changed stays).
"""
import os
import re

KNOWN = {}

RETIRED = {
    # Forza Horizon's motion blur threshold, set below its stencil 0x80 so the ground in a car's
    # contact shadow blurs as on the console. Texels now read as Xenos returns them, rounded to 16
    # fractional bits, and 0x80 is the game's own threshold (SDK patch 0049, Xenia canary's
    # model; docs/NOTES.md "A car's contact shadow in the motion blur").
    '4D5309C9': [('pixel_constant_override', '"E68FE55ECD482EC5:c253.w=0.5"')],
}


def _title_id(work):
    import json
    try:
        prep = json.load(open(os.path.join(work, 'prepare.json'), encoding='utf-8'))
    except (OSError, ValueError):
        prep = {}
    return ((prep.get('xex') or {}).get('title_id') or '').upper()


def apply(work, settings_files, log=print):
    """Sets KNOWN's [rexnative] keys for the prepared game in each settings file that has none,
    and takes RETIRED's out where they still hold the value written."""
    from .deck import toml_set
    tid = _title_id(work)
    tweaks = KNOWN.get(tid) or []
    retired = RETIRED.get(tid) or []
    if not tweaks and not retired:
        return []
    name = os.path.basename(os.path.normpath(work))
    for path in settings_files:
        if not os.path.exists(path):
            continue
        text = open(path, encoding='utf-8').read()
        new = text
        for key, value in tweaks:
            if not re.search(r'(?m)^\s*%s\s*=' % re.escape(key), new):
                new = toml_set(new, 'rexnative', key, value)
        for key, value in retired:
            new = re.sub(r'(?m)^[ \t]*%s[ \t]*=[ \t]*%s[ \t]*\r?\n?' % (re.escape(key), re.escape(value)),
                         '', new)
        if new != text:
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(new)
            if retired:
                log('%s: %s taken out of %s (the renderer does it now)' % (
                    name, ', '.join(k for k, _ in retired), os.path.basename(path)))
    if tweaks:
        log('%s: picture tweaks %s' % (name, ', '.join('%s = %s' % kv for kv in tweaks)))
    return tweaks
