"""The adapter: a reference port's game layer -> another game's.

  target = {'name', 'title', 'generated', 'xex'}
  refs   = {name: profile} (profile.extract of each hand-made port, with its generated tree)

1. Picks the references (same D3D library version first, then LTCG vs not, then the closest
   XDK build); the best is primary, the others fill the functions it could not find and
   cross-check the ones it did.
2. Matches the reference's functions (matcher.py), works out Resolve's hook and arguments
   (abi.py), the device offsets (offsets.py) and the default clear colour (constants.py).
3. Applies <out>/overrides.json, if any (choices made by hand after checking the report).
"""
import glob
import json
import os
import re

from . import abi, constants, listing, matcher, offsets, profile as prof, xex
from .i18n import tr

EMIT = ('high', 'medium')  # confidences written to the layer; 'low' ones are only reported
# The defaults' keys before the options were renamed (pgr4_* -> rexnative_*).
OLD_DEFAULT_KEYS = {'pgr4_render_height': 'render_height', 'pgr4_native_occlusion': 'occlusion',
                    'pgr4_native_display_aspect': 'display_aspect', 'pgr4_native_primary': 'enabled'}
# d3d_surface.inc category of each hooked function; the others are kDraw.
HOOK_CATS = {'Swap': 'kSwap', 'Resolve': 'kResolve', 'Clear': 'kClear', 'ClearF': 'kClear',
             'BeginTiling': 'kTiling', 'EndTiling': 'kTiling', 'QueryIssue': 'kQuery',
             'QueryGetData': 'kQuery', 'CreateDevice': 'kDevice', 'MakeSpace': 'kPush',
             'StartSegment': 'kPush', 'SwitchSegment': 'kPush', 'RestoreSegment': 'kPush',
             'RestoreSegment13392': 'kPush', 'RestorePush': 'kPush',
             'RunCommandBuffer': 'kOther'}
# Named functions that never get a D3D_FN row: hooked by symbol (ShaderLoad, ImmediateVs), or
# running inside a hooked one (ResolveInner).
NOT_HOOKED = ('ShaderLoad', 'ImmediateVs', 'ResolveInner', 'XInputGetState')


def score_reference(target_info, ref_profile):
    lib, ver = xex.d3d_library(target_info)
    rlib, rver = xex.d3d_library(ref_profile['xex'])
    s, why = 0.0, []
    if ver and rver == ver:
        s += 100
        why.append(tr('why_same_xdk', ver))
    else:
        b, rb = xex.xdk_build(ver), xex.xdk_build(rver)
        if b and rb:
            s -= abs(b - rb) / 100.0
        why.append(tr('why_xdk', rver, ver))
    if (rlib == 'D3D9LTCG') == (lib == 'D3D9LTCG'):
        s += 10
        why.append(tr('why_both', tr('why_ltcg') if lib == 'D3D9LTCG' else tr('why_no_ltcg')))
    else:
        why.append(tr('why_libs', rlib, lib))
    known = sum(1 for v in ref_profile['functions'].values() if v)
    s += known / 100.0
    return s, why


def named_functions(p):
    named = {a: n for a, n, c in p['surface']}
    for n, a in p['functions'].items():
        if a:
            named[a] = n
    return named


def run_matcher(R, T, p):
    return matcher.Matcher(R, T, named_functions(p)).run()


def xinput_get_state(T, generated, funcs):
    """XInputGetState(user, state), the XDK's pad read: three instructions, `mr r5,r4`,
    `li r4,1` (XINPUT_FLAG_GAMEPAD) and a branch to the XamInputGetState import, whose address
    the generated code's import table gives (`{ 0x82B08504, __imp__XamInputGetState }`). The
    settings HUD hooks it (rexnative_d3d_hooks.cpp) to keep its Back+Start chord from the game."""
    row = funcs['XInputGetState']
    imports = set()
    for path in glob.glob(os.path.join(generated, '*.cpp')):
        name = os.path.basename(path)
        if not (name.endswith('_init.cpp') or name.endswith('_register.cpp')):
            continue
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        imports.update(int(a, 16) for a in re.findall(
            r'0x([0-9A-Fa-f]{8}),\s*__imp__XamInputGetState\b', text))
    if not imports:
        row.update(detail='no XamInputGetState import in the generated code')
        return
    found = []
    for f, insns in T.funcs.items():
        if len(insns) != 3:
            continue
        texts = [t.replace(' ', '') for _, t in insns]
        if texts[0] != 'mrr5,r4' or texts[1] != 'lir4,1' or not texts[2].startswith('b0x'):
            continue
        if int(texts[2][3:], 16) in imports:
            found.append(f)
    if len(found) == 1:
        row.update(address=found[0], how='pattern', confidence='high',
                   detail='mr r5,r4; li r4,1; b XamInputGetState')
    elif found:
        row.update(detail='%d functions of its shape: %s' % (
            len(found), ', '.join('%08X' % f for f in sorted(found))))
    else:
        row.update(detail='no function of its shape (mr r5,r4; li r4,1; b XamInputGetState)')


def adapt(target, refs, overrides=None, log=print):
    info = xex.read(target['xex']) if target.get('xex') else None
    lib, ver = xex.d3d_library(info) if info else (None, None)
    T = listing.load(target['generated'])
    order = []
    for name, p in refs.items():
        s, why = score_reference(info, p) if info else (0.0, [tr('why_no_xex')])
        order.append((s, name, why))
    order.sort(reverse=True)
    if not order:
        raise SystemExit('rexnative: no reference')
    log('target %s: %s %s, %d functions' % (target['name'], lib, ver, len(T.funcs)))
    runs = []
    for s, name, why in order:
        p = refs[name]
        R = listing.load(p['generated'])
        m = run_matcher(R, T, p)
        log('  reference %s (score %.1f: %s): %d of %d matched' % (
            name, s, '; '.join(why), sum(1 for a in m.named if a in m.map), len(m.named)))
        runs.append({'name': name, 'score': s, 'why': why, 'profile': p, 'R': R, 'm': m})
    primary = runs[0]
    p, R, m = primary['profile'], primary['R'], primary['m']

    # -- functions ------------------------------------------------------------------------
    funcs = {}
    for canon in prof.FUNCTIONS:
        row = {'address': None, 'how': 'not found', 'confidence': None, 'detail': '',
               'ref': None, 'ref_address': None, 'cross': {}}
        for run in runs:
            ra = run['profile']['functions'].get(canon)
            if not ra:
                continue
            mm = run['m'].map.get(ra)
            if mm is None:
                cand = run['m'].candidates.get(ra) or [
                    (sc, t, 'listing %.2f' % sc) for sc, t in run['m'].weak.get(ra, [])]
                if cand and row['address'] is None and not row['detail']:
                    row['detail'] = 'candidates (%s): %s' % (run['name'], ', '.join(
                        '%08X %s' % (t, why) for sc, t, why in cand[:3]))
                continue
            if row['address'] is None and mm.confidence() in EMIT:
                row.update(address=mm.target, how=mm.how, confidence=mm.confidence(),
                           detail=mm.detail, ref=run['name'], ref_address=ra)
            else:
                row['cross'][run['name']] = (mm.target, mm.how, mm.confidence())
        funcs[canon] = row

    indexed_up(T, funcs)  # bounded by the DrawVertices matched, whatever it turns out to be
    draw_arity(T, funcs)
    create_device_arity(T, funcs)
    xinput_get_state(T, target['generated'], funcs)
    unique_addresses(funcs)

    # Resolve: which function to hook and where its arguments are. Its signature changed between
    # XDK builds (a public wrapper over an inner function with more arguments by 6995-11626,
    # inlined again by LTCG), so it comes from the reference whose Resolve looks most like this
    # game's, which need not be the primary one (Perfect Dark's 9328 and Super Meat Boy's 11626
    # are From Dust's shape, not PGR4's).
    candidates = []
    for run in runs:
        rk = abi.resolve_abi(run['R'], T, run['profile'], run['m'].map)
        if not rk:
            continue
        pf = run['profile']['functions']
        ref_fn = pf.get('ResolveInner') or pf.get('Resolve')
        tgt_fn = rk['inner'] or abi.thin_wrapper(T, rk['hook']) or rk['hook']
        sim = matcher.ratio(run['R'][ref_fn], T[tgt_fn]) if ref_fn in run['R'] and tgt_fn in T else 0
        candidates.append((sim, run['name'], rk))
    candidates.sort(key=lambda c: c[0], reverse=True)
    ra, ra_ref = (candidates[0][2], candidates[0][1]) if candidates else (None, None)
    if ra:
        ra['steps'].append('Resolve from the reference %s (listing %.2f%s)' % (
            ra_ref, candidates[0][0], ''.join(', %s %.2f' % (n, s) for s, n, _ in candidates[1:])))
        r = funcs['Resolve']
        r.update(address=ra['hook'], ref=ra_ref,
                 ref_address=refs[ra_ref]['functions']['Resolve'])
        if r['how'] in ('not found', 'wrapper'):
            r.update(how='wrapper' if ra['inner'] == ra['hook'] else 'public wrapper',
                     confidence='medium', detail='; '.join(ra['steps']))
        elif ra['inner'] and ra['inner'] != ra['hook']:
            r.update(how='public wrapper', detail='; '.join(ra['steps']))
        inner = ra['inner'] if ra['inner'] and ra['inner'] != ra['hook'] else None
        if inner is None:
            # What the hooked function wraps (through trampolines), else the matcher's.
            inner = abi.thin_wrapper(T, ra['hook'])
            mi = funcs['ResolveInner'].get('address')
            if inner is None and mi and mi != ra['hook']:
                inner = mi
        funcs['ResolveInner'].update(
            address=inner, how='abi' if inner else 'same as Resolve',
            confidence=r['confidence'] if inner else None,
            detail='the function the public Resolve wraps' if inner else '')

    # -- argument order of the hooked draw functions (LTCG builds drop / move arguments) ----------
    call = {}
    for canon in abi.CALL_ARG_FUNCTIONS:
        tgt_fn = funcs[canon]['address']
        if not tgt_fn or tgt_fn not in T:
            continue
        best_run = None
        for run in runs:
            ref_fn = run['profile']['functions'].get(canon)
            if not ref_fn or ref_fn not in run['R']:
                continue
            sim = matcher.ratio(run['R'][ref_fn], T[tgt_fn])
            if best_run is None or sim > best_run[0]:
                best_run = (sim, run, ref_fn)
        if best_run is None:
            continue
        sim, run, ref_fn = best_run
        ref_args = (run['profile'].get('call_args') or {}).get(canon)
        regs, evidence = abi.call_args(run['R'], T, ref_fn, tgt_fn, ref_args)
        uncertain = False
        if regs != list(range(3, 11)) and sim < 0.85:
            # A remap read from a weak match is worse than none (garbage counts hang the GPU):
            # the public order, flagged in the report.
            uncertain, evidence = True, evidence + ['not applied: listing %.2f < 0.85' % sim]
            regs = list(range(3, 11))
        call[canon] = {'args': regs, 'ref': run['name'], 'listing': round(sim, 2),
                       'evidence': evidence, 'uncertain': uncertain}

    # -- d3d_surface.inc: the reference's entries found with a usable confidence ----------------
    surface, seen, dropped = [], set(), []
    for run in runs:
        for a, name, cat in run['profile']['surface']:
            if any(e[1] == name for e in surface):
                continue
            mm = run['m'].map.get(a)
            if mm is None or mm.how == 'wrapper' or mm.confidence() not in EMIT:
                if run is primary:
                    dropped.append((a, name, mm.how if mm else 'not found',
                                    mm.detail if mm else ''))
                continue
            if mm.target in seen:
                if run is primary:
                    dropped.append((a, name, 'same function as another entry', '%08X' % mm.target))
                continue
            seen.add(mm.target)
            surface.append([mm.target, name, cat, mm.how, mm.confidence(),
                            run['name'] if run is not primary else ''])
    # Every hooked function must be in the surface (its kFn_ id); add the missing ones. A matched
    # RunCommandBuffer is not: hooking it changes how recorded command buffers reach the renderer
    # (kDefaultCmdBuf), a choice for overrides.json; a reference's row for it still comes through.
    for canon in prof.FUNCTIONS:
        a = funcs[canon]['address']
        if a and a not in seen and canon not in NOT_HOOKED + ('RunCommandBuffer',):
            surface.append([a, canon, HOOK_CATS.get(canon, 'kDraw'), funcs[canon]['how'],
                            funcs[canon]['confidence'], 'hook'])
            seen.add(a)
    # The inner Resolve of a hooked public wrapper runs inside it: hooked too (a reference may name
    # it Resolve, as Iron Brigade's does), every resolve would reach the renderer twice.
    inner = funcs['ResolveInner']['address']
    if inner and inner != funcs['Resolve']['address']:
        surface = [e for e in surface if e[0] != inner]
    surface.sort()

    # -- device offsets ---------------------------------------------------------------------
    pairs = [(r, mm.target) for r, mm in m.map.items() if mm.confidence() in EMIT]
    v = offsets.votes(R, T, pairs)
    offs = offsets.map_offsets(p['offsets'], v, R, list(m.named))

    # -- default clear colour ---------------------------------------------------------------
    clear = None
    hooked_inner = funcs['ResolveInner']['address'] or funcs['Resolve']['address']
    if hooked_inner and hooked_inner in T:
        clear = constants.default_clear_color(T[hooked_inner])
    ref_clear_check = None
    ref_resolve = p['functions'].get('ResolveInner') or p['functions'].get('Resolve')
    if ref_resolve and ref_resolve in R:
        own = constants.default_clear_color(R[ref_resolve])
        ref_clear_check = {
            'profile': p.get('clear_color'),
            'found': own,
            'consistent': bool(own and p.get('clear_color') and
                               own['address'] == p['clear_color']['address'] and
                               own['vector'] == p['clear_color']['vector']),
        }

    result = {
        'target': {'name': target['name'], 'title': target.get('title', target['name']),
                   'generated': target['generated'], 'xex': info, 'd3d': [lib, ver],
                   'functions_in_tree': len(T.funcs)},
        'references': [{'name': r['name'], 'score': r['score'], 'why': r['why'],
                        'matched': sum(1 for a in r['m'].named if a in r['m'].map),
                        'wanted': len(r['m'].named)} for r in runs],
        'primary': primary['name'],
        'library_ranges': [['%08X' % lo, '%08X' % hi] for lo, hi in m.target_ranges],
        'functions': funcs,
        'surface': surface,
        'surface_dropped': dropped,
        'offsets': offs,
        'clear_color': clear,
        'reference_clear_check': ref_clear_check,
        'resolve_abi': ra,
        'call_args': call,
        # Not in the code: the renderer learns it at run time (scene.h, REXNATIVE_SCENE_AUTO);
        # the reference's values stay as the fallback. overrides.json "scene" (measured with
        # rexnative scene) fixes it instead.
        'scene': dict(p['scene'], auto=True),
        # The game's own frame height (PGR4's 800 is its Steam Deck mode, made by its own
        # hooks), occlusion queries on the host off (From Dust hung with them; a new game's
        # query hooks are often not found), the console's 16:9.
        # Recorded command buffers are not replayed (cmdbuf 0) unless overrides.json says so.
        'defaults': {'enabled': True,
                     'render_height': p['scene'].get('height', 720),
                     'occlusion': False,
                     'display_aspect': '16.0 / 9.0',
                     'cmdbuf': 0},
        'devices': {
            'follow_swap_device_only': False,
            'configure_at_first_device': funcs['CreateDevice']['address'] is None,
        },
        'overrides': {},
    }
    if overrides:
        apply_overrides(result, overrides)
    return result


_STORE_LIKE = ('st', 'cmp', 'tw', 'td', 'mtctr', 'mtlr', 'mtspr', 'mtcrf', 'b', 'dcb', 'icb')


def reads_argument(T, f, reg):
    """Whether function f reads `reg` before writing it: an argument it takes."""
    for _, text in T[f]:
        parts = text.replace(',', ' ').replace('(', ' ').replace(')', ' ').split()
        if not parts:
            continue
        op, ops = parts[0], parts[1:]
        if op.startswith(_STORE_LIKE):
            if reg in ops:
                return True
            continue
        if reg in ops[1:]:
            return True
        if ops and ops[0] == reg:
            return False
    return False


def draw_arity(T, funcs):
    """DrawVertices(device, primitive, start vertex, count) takes four arguments,
    DrawIndexedVertices(device, primitive, base vertex, start index, index count) five. A
    DrawVertices found that reads r7 is the game's DrawIndexedVertices (Perfect Dark: its callers
    pass a triangle list, base 0, a start index and 3n indices; it has no DrawVertices, the linker
    dropped it), and drawn as DrawVertices every indexed draw came out as stretched triangles."""
    dv, di = funcs.get('DrawVertices'), funcs.get('DrawIndexedVertices')
    if not dv or not dv.get('address') or di is None:
        return
    a = dv['address']
    try:
        if not reads_argument(T, a, 'r7'):
            return
    except KeyError:
        return
    detail = '%08X reads r7: five arguments, DrawIndexedVertices\'s (was matched as DrawVertices)' % a
    # Its own match, unless that address is another function's (Perfect Dark's matcher gave it
    # 823382C0, BeginIndexedVertices).
    others = {r.get('address') for n, r in funcs.items()
              if n not in ('DrawVertices', 'DrawIndexedVertices') and r.get('address')}
    if not di.get('address') or di['address'] == a or di['address'] in others:
        di.update(address=a, how='arity', confidence=dv.get('confidence') or 'medium', detail=detail)
    dv.update(address=None, how='not found', confidence=None, detail=detail)


def create_device_arity(T, funcs):
    """Direct3D_CreateDevice(adapter, device type, window, flags, parameters, ppDevice) returns
    the device through r8, where the hook reads it. A CreateDevice found that never reads r8 is
    not that function: Brothers - A Tale of Two Sons (D3D 2.0.21250) creates its device in a
    function of the presentation parameters alone, which keeps it in a global, and the hook read
    the device from whatever r8 held (0x20: the game crashed on its first call). Without one the
    renderer takes the first device the D3D calls use (configure_at_first_device)."""
    cd = funcs.get('CreateDevice')
    if not cd or not cd.get('address'):
        return
    a = cd['address']
    try:
        if reads_argument(T, a, 'r8'):
            return
    except KeyError:
        return
    cd.update(address=None, how='not found', confidence=None,
              detail='%08X does not read r8 (ppDevice): not Direct3D_CreateDevice; the first '
                     'device seen instead' % a)


def indexed_up(T, funcs):
    """BeginIndexedVertices and DrawIndexedVerticesUP, when no reference has them (the linker
    drops them from games that never draw with user-pointer indices, as From Dust and PGR4).
    The XDK keeps its draw builders in source order: DrawVerticesUP, BeginIndexedVertices,
    DrawIndexedVerticesUP, DrawVertices. Between the two found ones, DrawIndexedVerticesUP is the
    function that calls exactly one other function of that range, BeginIndexedVertices, which
    reserves its space through MakeSpace. Calls may go through trampolines (Jet Set Radio, Super
    Meat Boy). A game may keep BeginIndexedVertices alone (Bionic Commando Rearmed): then the
    only function between them that calls MakeSpace and tests bit 2 of its index format (r8,
    32-bit indices)."""
    rows = [funcs[n] for n in ('BeginIndexedVertices', 'DrawIndexedVerticesUP')]
    if any(r['address'] for r in rows):
        return
    up, dv = funcs['DrawVerticesUP']['address'], funcs['DrawVertices']['address']
    space = funcs['MakeSpace']['address']
    if not (up and dv and space and up < dv):
        return
    between = [f for f in T.starts if up < f < dv]

    def callees(f):
        return {abi.trampoline(T, c) or c for _, c, kind in T.calls.get(f, ()) if kind == 'bl'}

    makes_space = [f for f in between if space in callees(f)]
    found = []
    for y in between:
        inner = {c for c in callees(y) if c in between and c != y}
        if len(inner) == 1 and next(iter(inner)) in makes_space:
            found.append((next(iter(inner)), y))
    where = 'between DrawVerticesUP %08X and DrawVertices %08X' % (up, dv)
    if len(found) == 1:
        x, y = found[0]
        detail = '%s: %08X calls only %08X, which calls MakeSpace' % (where, y, x)
        for row, a in zip(rows, (x, y)):
            row.update(address=a, how='position', confidence='high', detail=detail)
        return
    if found:
        return
    index32 = re.compile(r'^rlwinm\. r\d+,(r\d+),0,29,29$')

    def tests_index32(f):
        # r8 or a copy of it (`mr r24,r8` in Bionic Commando Rearmed).
        regs = {'r8'}
        for _, t in T[f]:
            m = re.match(r'^mr (r\d+),(r\d+)$', t)
            if m and m.group(2) in regs:
                regs.add(m.group(1))
            m = index32.match(t)
            if m and m.group(1) in regs:
                return True
        return False

    alone = [f for f in makes_space if tests_index32(f)]
    if len(alone) == 1:
        rows[0].update(address=alone[0], how='position', confidence='high',
                       detail='%s: %08X, the only one that calls MakeSpace and tests the index '
                              'format for 32-bit indices (no DrawIndexedVerticesUP)' % (where, alone[0]))


# Functions that may share an address: a public wrapper that is its own inner function, and the
# two names of the segment restore.
SAME_ADDRESS_OK = ({'Resolve', 'ResolveInner'}, {'RestoreSegment', 'RestoreSegment13392'})


def unique_addresses(funcs):
    """No address for two functions (the renderer switches on them: Perfect Dark's position-found
    BeginIndexedVertices was its DrawIndexedVertices, Bionic Commando Rearmed's RunCommandBuffer
    match its BeginIndexedVertices). The surer one keeps it: higher confidence, then one matched
    to a reference over one found by position."""
    rank = {'high': 3, 'medium': 2, 'low': 1}
    by = {}
    for canon, row in funcs.items():
        if row.get('address'):
            by.setdefault(row['address'], []).append(canon)
    for a, names in by.items():
        if len(names) < 2 or any(set(names) <= s for s in SAME_ADDRESS_OK):
            continue
        names.sort(key=lambda n: (rank.get(funcs[n].get('confidence'), 0),
                                  funcs[n].get('how') != 'position'), reverse=True)
        keep = names[0]
        for n in names[1:]:
            if any({keep, n} <= s for s in SAME_ADDRESS_OK):
                continue
            funcs[n].update(address=None, how='not found', confidence=None,
                            detail='%08X is %s (%s, %s)' % (a, keep, funcs[keep].get('how'),
                                                           funcs[keep].get('confidence')))


def apply_overrides(result, ov):
    """overrides.json: {"functions": {name: "XXXXXXXX" | null}, "scene": {...},
    "defaults": {...}, "devices": {...}, "clear_color": {"address": "...", "vector": bool},
    "resolve_args": [8 registers]}"""
    for name, val in ov.get('functions', {}).items():
        row = result['functions'].setdefault(name, {})
        old = row.get('address')
        new = int(val, 16) if val else None
        if old and old != new:
            # Not hooked any more under any name: out of d3d_surface.inc too.
            result['surface'] = [e for e in result['surface'] if e[0] != old]
        row.update(address=new, how='override', confidence='override', detail='overrides.json')
        # An address set by hand is hooked: its D3D_FN row, unless a reference's row already
        # names that function (it keeps the reference's name and category).
        if (new and name in prof.FUNCTIONS and name not in NOT_HOOKED
                and not any(e[0] == new for e in result['surface'])):
            result['surface'].append([new, name, HOOK_CATS.get(name, 'kDraw'), 'override',
                                      'override', 'hook'])
            result['surface'].sort()
    if 'scene' in ov:
        # A measured scene: the renderer uses it as it is, unless the file says "auto": true.
        result['scene'].update(ov['scene'])
        result['scene']['auto'] = bool(ov['scene'].get('auto', False))
    for old, new in OLD_DEFAULT_KEYS.items():  # overrides.json written before ReXNative 4
        if old in ov.get('defaults', {}):
            ov.setdefault('defaults', {}).setdefault(new, ov['defaults'].pop(old))
    for key in ('defaults', 'devices'):
        result[key].update(ov.get(key, {}))
    if 'clear_color' in ov:
        c = ov['clear_color']
        result['clear_color'] = {'address': int(c['address'], 16), 'vector': bool(c['vector'])}
    if 'resolve_args' in ov and result['resolve_abi']:
        result['resolve_abi']['args'] = ov['resolve_args']
    for name, regs in ov.get('call_args', {}).items():
        result.setdefault('call_args', {})[name] = {'args': regs, 'ref': 'overrides.json',
                                                    'listing': None, 'evidence': []}
    result['overrides'] = ov


def load_overrides(out_dir):
    path = os.path.join(out_dir, 'overrides.json')
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    return None
