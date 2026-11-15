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
import json
import os

from . import abi, constants, listing, matcher, offsets, profile as prof, xex
from .i18n import tr

EMIT = ('high', 'medium')  # confidences written to the layer; 'low' ones are only reported
# The defaults' keys before the options were renamed (pgr4_* -> rexnative_*).
OLD_DEFAULT_KEYS = {'pgr4_render_height': 'render_height', 'pgr4_native_occlusion': 'occlusion',
                    'pgr4_native_display_aspect': 'display_aspect', 'pgr4_native_primary': 'enabled'}


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
    # Every hooked function must be in the surface (its kFn_ id); add the missing ones.
    cats = {'Swap': 'kSwap', 'Resolve': 'kResolve', 'Clear': 'kClear', 'ClearF': 'kClear',
            'BeginTiling': 'kTiling', 'EndTiling': 'kTiling', 'QueryIssue': 'kQuery',
            'QueryGetData': 'kQuery', 'CreateDevice': 'kDevice', 'MakeSpace': 'kPush',
            'StartSegment': 'kPush', 'SwitchSegment': 'kPush', 'RestoreSegment': 'kPush',
            'RestoreSegment13392': 'kPush', 'RestorePush': 'kPush'}
    for canon in prof.FUNCTIONS:
        a = funcs[canon]['address']
        if a and a not in seen and canon not in ('ShaderLoad', 'ImmediateVs', 'ResolveInner',
                                                 'RunCommandBuffer'):
            surface.append([a, canon, cats.get(canon, 'kDraw'), funcs[canon]['how'],
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
        'defaults': {'enabled': True,
                     'render_height': p['scene'].get('height', 720),
                     'occlusion': False,
                     'display_aspect': '16.0 / 9.0'},
        'devices': {
            'follow_swap_device_only': False,
            'configure_at_first_device': funcs['CreateDevice']['address'] is None,
        },
        'overrides': {},
    }
    if overrides:
        apply_overrides(result, overrides)
    return result


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
