"""Which DLSS 5 look suits a game (renderer/src/native/dlss5_looks.h): its default, written into
the game's settings by `project` (the settings HUD changes it, and a choice made there stays).

The pick: a game checked by eye (KNOWN, by title id), else a word of its name (KEYWORDS: the
cel-shaded, anime and 2D games, stylized 3D, racing), else cinematic, the look for realistic
games. Looks, from the gentlest: cel (cel-shaded, anime, 2D: flat colours kept), stylized (cartoon
and painterly 3D: hues kept), natural, cinematic, cinematic2 (two passes: racing, after the
modding community's PS2 tests), strong.
"""
import json
import os
import re

LOOKS = ('cinematic', 'cinematic2', 'natural', 'stylized', 'cel', 'strong', 'custom')

# Title id -> look, for the games prepared so far (2026-11-18).
KNOWN = {
    '58411247': 'cel',         # Jet Set Radio: cel-shaded
    '58410847': 'cel',         # Castlevania: Symphony of the Night: 2D sprites
    '584109BE': 'cel',         # Spelunky: 2D
    '58410A5A': 'cel',         # Super Meat Boy: 2D
    '584113C3': 'cel',         # Dungeons & Dragons: Chronicles of Mystara: 2D sprites
    '58410841': 'stylized',    # 3D Ultra Minigolf Adventures: cartoon
    '584108F1': 'stylized',    # Bionic Commando: Rearmed: 2.5D, saturated
    '584112A5': 'stylized',    # Brothers: A Tale of Two Sons: painterly
    '58411252': 'stylized',    # Double Dragon: Neon: neon cartoon
    '584111F7': 'stylized',    # Minecraft: blocks
    '584111EB': 'stylized',    # Orcs Must Die!: cartoon
    '434D07D5': 'stylized',    # Overlord: storybook fantasy
    '434D0809': 'stylized',    # Overlord II
    '4D5307F1': 'stylized',    # Fable II: painterly fantasy
    '545407F8': 'cinematic2',  # Midnight Club: Los Angeles: racing
    '465307E4': 'cinematic',   # Dark Souls II
    '4D5309B1': 'cinematic',   # Halo: Combat Evolved Anniversary
    '584109C2': 'cinematic',   # Perfect Dark (XBLA)
    '54540859': 'cinematic',   # Top Spin 4
}

# Words of a game's name (lower case) -> look; the first list that matches wins.
KEYWORDS = [
    ('cel', ['jet set radio', 'borderlands', 'naruto', 'dragon ball', 'one piece', 'tales of',
             'eternal sonata', 'simpsons', 'crackdown', 'afro samurai', 'catherine',
             'prince of persia', 'el shaddai', 'castle crashers', 'xiii', 'street fighter',
             'marvel vs', 'metal slug', 'castlevania', 'spelunky', 'meat boy', 'braid', 'limbo',
             'elysian tail', 'mega man', 'rayman', 'scott pilgrim', 'battleblock', 'shank',
             'guilty gear', 'blazblue', 'persona', 'ninja gaiden 3', 'sonic', 'okami',
             'cel damage', 'viewtiful', 'zone of the enders', 'darkstalkers', 'king of fighters',
             'mystara', 'contra', 'outland', 'trials', 'mark of the ninja', 'fez']),
    ('stylized', ['overlord', 'orcs must die', 'minecraft', 'diablo', 'fable', 'viva pinata',
                  'banjo', 'kameo', 'brothers', 'double dragon', 'minigolf', 'plants vs',
                  'torchlight', 'trine', 'lego', 'ratchet', 'spyro', 'crash', 'psychonauts',
                  'costume quest', 'brutal legend', 'bionic commando rearmed', 'world of warcraft',
                  'darksiders', 'kingdom hearts', 'dishonored', 'team fortress', 'portal',
                  'mirror\'s edge', 'alice', 'splosion', 'raskulls', 'peggle']),
    ('cinematic2', ['midnight club', 'forza', 'need for speed', 'burnout', 'project gotham',
                    'dirt', 'grid', 'ridge racer', 'test drive', 'split/second', 'blur',
                    'motorstorm', 'f1 20', 'gran turismo', 'race driver', 'colin mcrae', 'nascar',
                    'motogp', 'pure', 'sega rally', 'hot pursuit', 'driver: san francisco']),
    ('natural', ['deus ex']),
]


def look_for(work):
    """(look, why) for the prepared game in work."""
    try:
        prep = json.load(open(os.path.join(work, 'prepare.json'), encoding='utf-8'))
    except (OSError, ValueError):
        prep = {}
    tid = ((prep.get('xex') or {}).get('title_id') or '').upper()
    if tid in KNOWN:
        return KNOWN[tid], 'checked by eye (%s)' % tid
    from .deck import title_of
    name = ' ' + re.sub(r'[^a-z0-9/\' ]+', ' ', title_of(work).lower()) + ' '
    for look, words in KEYWORDS:
        for w in words:
            if ' ' + w in name:
                return look, 'its name has "%s"' % w
    return 'cinematic', 'the default (realistic)'


def apply(work, settings_files, log=print):
    """Sets [rexnative] dlss5_look in each settings file that has none (a look the player chose in
    the HUD stays); returns the look."""
    from .deck import toml_set
    look, why = look_for(work)
    for path in settings_files:
        if not os.path.exists(path):
            continue
        text = open(path, encoding='utf-8').read()
        if re.search(r'(?m)^\s*dlss5_look\s*=', text):
            continue
        text = toml_set(text, 'rexnative', 'dlss5_look', '"%s"' % look)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    log('%s: DLSS 5 look %s (%s)' % (os.path.basename(os.path.normpath(work)), look, why))
    return look
