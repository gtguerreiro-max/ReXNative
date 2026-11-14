"""Reports (Markdown, in the language i18n.py selects: English by default, --lang pt for
Portuguese): the adapter's report and the validation against a hand-made layer."""
import collections
import difflib

from . import emit, profile as prof
from .i18n import table, tr


def _h(a):
    return '`%08X`' % a if a else '—'


def _how(h):
    return table('how').get(h, h)


def _conf(c):
    return table('conf').get(c, c)


def adapter_report(res):
    t = res['target']
    info = t.get('xex') or {}
    L = []
    w = L.append
    w(tr('r_title', t['title']))
    w('')
    w(tr('r_exe', info.get('title_id'), info.get('version'), info.get('pe_name'),
         ' '.join(x for x in t['d3d'] if x)))
    w(tr('r_generated', t['generated'], t['functions_in_tree']))
    w(tr('r_library', ', '.join('`%s-%s`' % tuple(r) for r in res['library_ranges'])))
    w('')
    w(tr('r_refs'))
    w('')
    w(tr('r_refs_head'))
    w('|---|---|---|---|')
    for r in res['references']:
        w('| %s%s | %.1f | %s | %s |' % (r['name'], tr('r_primary') if r['name'] == res['primary']
                                         else '', r['score'], '; '.join(r['why']),
                                         tr('r_of', r['matched'], r['wanted'])))
    w('')
    w(tr('r_funcs'))
    w('')
    w(tr('r_funcs_head'))
    w('|---|---|---|---|---|')
    check = []
    for canon in prof.FUNCTIONS:
        r = res['functions'][canon]
        cross = ''
        if r.get('cross'):
            parts = []
            for name, (a, how, conf) in r['cross'].items():
                same = a == r['address'] or (canon == 'Resolve' and a == res['functions'][
                    'ResolveInner']['address'])
                parts.append('%s: %s %s' % (name, '%08X' % a,
                                            tr('r_agrees') if same else tr('r_disagrees')))
                if not same:
                    check.append(tr('c_disagree', canon, name, a,
                                    '%08X' % r['address'] if r['address'] else '—'))
            cross = ' (' + '; '.join(parts) + ')'
        w('| %s | %s | %s | %s | %s%s |' % (canon, _h(r['address']), _how(r['how']),
                                         _conf(r['confidence']), r['detail'].replace('|', '/'),
                                         cross))
        if r['address'] and r['confidence'] == 'medium':
            check.append(tr('c_medium', canon, r['address'], r['detail']))
        if not r['address'] and canon not in ('ResolveInner',):
            check.append(tr('c_not_found', canon, (' — ' + r['detail']) if r['detail'] else ''))
    w('')
    ra = res.get('resolve_abi')
    w('## Resolve')
    w('')
    if ra:
        w(tr('r_hook', ra['hook'], tr('r_inner', ra['inner'])
             if ra['inner'] and ra['inner'] != ra['hook'] else '',
             tr('r_public') if ra['abi'] == 'public' else tr('r_remapped')))
        for s in ra['steps']:
            w('- %s' % s)
        w('')
        w(tr('r_args_head'))
        w('|---|---|---|---|')
        for name, p, src, n, tot in ra['public']:
            w('| %s | %s | %s | %s |' % (name, p, src, ('%d/%d' % (n, tot)) if tot else
                                         (tr('r_assumed') if name in ra['assumed'] else '—')))
        if ra.get('overruled'):
            check.append(tr('c_overruled', ', '.join(ra['overruled'])))
        if ra['assumed']:
            check.append(tr('c_assumed', ', '.join(ra['assumed'])))
        if not ra['stack_ok']:
            check.append(tr('c_stack'))
    else:
        w(tr('r_no_hook'))
        check.append(tr('c_no_hook'))
    w('')
    cc = res.get('clear_color')
    w(tr('r_clear'))
    w('')
    if cc:
        w(tr('r_clear_found', cc['address'], tr('r_vector') if cc['vector'] else tr('r_scalar')))
    else:
        w(tr('r_not_found'))
        check.append(tr('c_no_clear'))
    rc = res.get('reference_clear_check')
    if rc and not rc['consistent']:
        def kind(c):
            return tr('w_vector') if c and c['vector'] else tr('w_scalar')
        w(tr('r_ref_inconsistent', res['primary'],
             '%08X' % rc['profile']['address'] if rc['profile'] else '—', kind(rc['profile']),
             '%08X' % rc['found']['address'] if rc['found'] else '—', kind(rc['found'])))
    w('')
    w(tr('r_offsets'))
    w('')
    w(tr('r_offsets_head'))
    w('|---|---|---|---|---|')
    for k, v in res['offsets'].items():
        mark = '**' if v['target'] != v['ref'] else ''
        w('| %s | %d | %s%d%s | %s | %s |' % (k, v['ref'], mark, v['target'], mark,
                                            ('%d/%d' % (v['votes'], v['total'])) if v['total'] else '—',
                                            v['how']))
        if 'split' in v['how']:
            check.append(tr('c_split', k))
    w('')
    w('## d3d_surface.inc')
    w('')
    cnt = collections.Counter((how, conf) for a, n, c, how, conf, src in res['surface'])
    w(tr('r_entries', len(res['surface']), ', '.join(
        '%d %s/%s' % (n, _how(h), _conf(c)) for (h, c), n in cnt.most_common())))
    w(tr('r_dropped', len(res['surface_dropped'])))
    w('')
    for a, n, how, det in res['surface_dropped'][:40]:
        w('  - `%08X` %s: %s %s' % (a, n, _how(how), det))
    if len(res['surface_dropped']) > 40:
        w(tr('r_more', len(res['surface_dropped']) - 40))
    w('')
    w(tr('r_scene'))
    w('')
    sc = res['scene']
    w(tr('r_scene_copied', sc.get('kSceneSurface', 0), sc.get('kSceneSurfaceMask', 0),
         sc.get('width'), sc.get('scaled_pitch'), sc.get('height')))
    w(tr('r_scene_how'))
    check.append(tr('c_scene'))
    dv = res['devices']
    if not dv.get('follow_swap_device_only'):
        check.append(tr('c_devices'))
    w('')
    w(tr('r_check'))
    w('')
    for c in check:
        w('- %s' % c)
    w('')
    return '\n'.join(L)


def _cmp(a, b):
    return tr('v_equal') if a == b else tr('v_different')


def validation_report(res, hand, ref_note=''):
    """Compares the adapter's result for a port with the port's hand-made profile. The stats
    keys are English whatever the language (validation.json)."""
    L = []
    w = L.append
    t = res['target']
    w(tr('v_title', t['title']))
    w('')
    if ref_note:
        w(ref_note)
        w('')
    w(tr('v_refs', ', '.join(r['name'] for r in res['references']), res['primary']))
    w('')
    stats = collections.Counter()
    w(tr('v_funcs'))
    w('')
    w(tr('v_funcs_head'))
    w('|---|---|---|---|---|')
    for canon in prof.FUNCTIONS:
        g = res['functions'][canon]['address']
        h = hand['functions'].get(canon)
        if canon == 'ResolveInner' and h is None and g == res['functions']['Resolve']['address']:
            g = None
        if g == h:
            key = 'equal'
        elif g and not h:
            key = 'only generated'
        elif h and not g:
            key = 'only hand-made'
        else:
            key = 'different'
        stats['functions ' + key] += 1
        verdict = tr({'equal': 'v_equal', 'only generated': 'v_only_gen',
                      'only hand-made': 'v_only_hand', 'different': 'v_different'}[key])
        r = res['functions'][canon]
        w('| %s | %s | %s | %s | %s %s |' % (canon, _h(g), _h(h), verdict, _how(r['how']),
                                            table('conf').get(r['confidence'], '')))
    w('')
    w('## Resolve')
    w('')
    ra = res.get('resolve_abi') or {}
    w(tr('v_args', ra.get('args'), hand['resolve_abi']['args'],
         _cmp(ra.get('args'), hand['resolve_abi']['args'])))
    cc, hc = res.get('clear_color'), hand.get('clear_color')

    def colour(c):
        if not c:
            return '—'
        return '`%08X` %s' % (c['address'], tr('w_vector') if c['vector'] else tr('w_scalar'))
    w(tr('v_clear', colour(cc), colour(hc),
         _cmp(cc and (cc['address'], cc['vector']), hc and (hc['address'], hc['vector']))))
    w('')
    w(tr('r_offsets'))
    w('')
    diff_offs = [(k, v['target'], hand['offsets'].get(k), v['how']) for k, v in res['offsets'].items()
                 if v['target'] != hand['offsets'].get(k)]
    same = len(res['offsets']) - len(diff_offs)
    w(tr('v_offsets_same', same, len(res['offsets'])))
    for k, g, h, how in diff_offs:
        w(tr('v_offset_diff', k, g, h, how))
    stats['offsets equal'] = same
    stats['offsets different'] = len(diff_offs)
    w('')
    w(tr('v_surface'))
    w('')
    gen = {n: a for a, n, c, how, conf, src in res['surface']}
    hm = {n: a for a, n, c in hand['surface']}
    agree = [n for n in gen if hm.get(n) == gen[n]]
    differ = [n for n in gen if n in hm and hm[n] != gen[n]]
    only_g = [n for n in gen if n not in hm]
    only_h = [n for n in hm if n not in gen]
    w(tr('v_surface_counts', len(agree), len(differ), len(only_g), len(only_h)))
    stats.update({'surface equal': len(agree), 'surface different': len(differ),
                  'surface only generated': len(only_g), 'surface only hand-made': len(only_h)})
    for n in differ:
        w(tr('v_surface_diff', n, gen[n], hm[n]))
    for n in only_h:
        w(tr('v_surface_hand', n, hm[n]))
    if only_g:
        w(tr('v_surface_gen', ', '.join('%s `%08X`' % (n, gen[n]) for n in only_g)))
    w('')
    w(tr('v_diff'))
    w('')
    a = emit.game_layer_h(emit.from_profile(hand), comments=False).splitlines()
    b = emit.game_layer_h(emit.from_result(res), comments=False).splitlines()
    d = list(difflib.unified_diff(a, b, tr('v_diff_hand'), tr('v_diff_gen'), lineterm='', n=0))
    w('```diff')
    L.extend(d or [tr('v_no_diff')])
    w('```')
    w('')
    return '\n'.join(L), stats
