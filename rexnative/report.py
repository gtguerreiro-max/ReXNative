"""Reports (Markdown, in Portuguese for the user): the adapter's report and the validation
against a hand-made layer."""
import collections
import difflib

from . import emit, profile as prof

CONF_PT = {'high': 'alta', 'medium': 'média', 'low': 'baixa', 'override': 'manual', None: '—'}
HOW_PT = {
    'exact': 'listagem igual', 'masked': 'listagem igual (números mascarados)',
    'region': 'igual, única na biblioteca', 'callgraph': 'chamada no mesmo sítio',
    'neighbour': 'vizinha de uma já encontrada', 'callees': 'chama as mesmas funções',
    'fuzzy': 'listagem parecida', 'wrapper': 'só existe a função interna',
    'public wrapper': 'wrapper público da função encontrada', 'abi': 'função interna do Resolve',
    'same as Resolve': 'a mesma do Resolve', 'not found': 'não encontrada', 'override': 'overrides.json',
}


def _h(a):
    return '`%08X`' % a if a else '—'


def adapter_report(res):
    t = res['target']
    info = t.get('xex') or {}
    L = []
    w = L.append
    w('# Camada gerada: %s' % t['title'])
    w('')
    w('- Executável: title id `%s`, versão `%s`, `%s`, biblioteca D3D **%s**' % (
        info.get('title_id'), info.get('version'), info.get('pe_name'), ' '.join(
            x for x in t['d3d'] if x)))
    w('- Código gerado: `%s` (%d funções)' % (t['generated'], t['functions_in_tree']))
    w('- Biblioteca D3D encontrada em: %s' % ', '.join('`%s-%s`' % tuple(r)
                                                      for r in res['library_ranges']))
    w('')
    w('## Referências')
    w('')
    w('| Referência | Pontuação | Porquê | Encontradas |')
    w('|---|---|---|---|')
    for r in res['references']:
        w('| %s%s | %.1f | %s | %d de %d |' % (r['name'], ' (primária)' if r['name'] == res['primary']
                                             else '', r['score'], '; '.join(r['why']),
                                             r['matched'], r['wanted']))
    w('')
    w('## Funções que o renderer usa')
    w('')
    w('| Função | Endereço | Como | Confiança | Detalhe |')
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
                parts.append('%s: %s %s' % (name, '%08X' % a, 'concorda' if same else '**discorda**'))
                if not same:
                    check.append('%s: %s diz `%08X`, a primária `%s`' % (
                        canon, name, a, '%08X' % r['address'] if r['address'] else '—'))
            cross = ' (' + '; '.join(parts) + ')'
        w('| %s | %s | %s | %s | %s%s |' % (canon, _h(r['address']), HOW_PT.get(r['how'], r['how']),
                                         CONF_PT.get(r['confidence'], r['confidence']),
                                         r['detail'].replace('|', '/'), cross))
        if r['address'] and r['confidence'] == 'medium':
            check.append('%s `%08X`: confiança média (%s)' % (canon, r['address'], r['detail']))
        if not r['address'] and canon not in ('ResolveInner',):
            check.append('%s: não encontrada%s' % (canon, (' — ' + r['detail']) if r['detail'] else ''))
    w('')
    ra = res.get('resolve_abi')
    w('## Resolve')
    w('')
    if ra:
        w('- Hook em `%08X`%s; ABI: **%s**.' % (ra['hook'], (' (função interna `%08X`)' % ra['inner'])
                                             if ra['inner'] and ra['inner'] != ra['hook'] else '',
                                             'pública' if ra['abi'] == 'public' else 'remapeada'))
        for s in ra['steps']:
            w('- %s' % s)
        w('')
        w('| Argumento público | Posição pública | Na entrada do hook | Votos |')
        w('|---|---|---|---|')
        for name, p, src, n, tot in ra['public']:
            w('| %s | %s | %s | %s |' % (name, p, src, ('%d/%d' % (n, tot)) if tot else
                                         ('assumido' if name in ra['assumed'] else '—')))
        if ra.get('overruled'):
            check.append('Resolve: a camada da referência põe %s a 0, mas o registo é usado como '
                         'argumento nas duas listagens: usado o registo' % ', '.join(ra['overruled']))
        if ra['assumed']:
            check.append('Resolve: sem uso alinhado para %s (assumido na posição pública)'
                         % ', '.join(ra['assumed']))
        if not ra['stack_ok']:
            check.append('Resolve: argumentos de pilha noutro sítio: o EntryArgs genérico não os '
                         'remapeia')
    else:
        w('- Não foi possível decidir o hook do Resolve.')
        check.append('Resolve: hook por decidir')
    w('')
    cc = res.get('clear_color')
    w('## Cor de clear por defeito (pClearColor nulo)')
    w('')
    if cc:
        w('- `%08X`, %s.' % (cc['address'], 'vector de 16 bytes (`lvx128`)' if cc['vector']
                             else 'um float repetido 4 vezes (`lfs`)'))
    else:
        w('- Não encontrada.')
        check.append('Cor de clear por defeito não encontrada')
    rc = res.get('reference_clear_check')
    if rc and not rc['consistent']:
        w('- **A referência (%s) está inconsistente**: o port diz `%s` (%s), mas a listagem da '
          'própria referência usa `%s` (%s).' % (
              res['primary'], '%08X' % rc['profile']['address'] if rc['profile'] else '—',
              'vector' if rc['profile'] and rc['profile']['vector'] else 'escalar',
              '%08X' % rc['found']['address'] if rc['found'] else '—',
              'vector' if rc['found'] and rc['found']['vector'] else 'escalar'))
    w('')
    w('## Offsets do device')
    w('')
    w('| Nome | Referência | Alvo | Votos | Nota |')
    w('|---|---|---|---|---|')
    for k, v in res['offsets'].items():
        mark = '**' if v['target'] != v['ref'] else ''
        w('| %s | %d | %s%d%s | %s | %s |' % (k, v['ref'], mark, v['target'], mark,
                                            ('%d/%d' % (v['votes'], v['total'])) if v['total'] else '—',
                                            v['how']))
        if 'split' in v['how']:
            check.append('offset %s: votos divididos' % k)
    w('')
    w('## d3d_surface.inc')
    w('')
    cnt = collections.Counter((how, conf) for a, n, c, how, conf, src in res['surface'])
    w('- %d entradas: %s.' % (len(res['surface']), ', '.join(
        '%d %s/%s' % (n, HOW_PT.get(h, h), CONF_PT.get(c, c)) for (h, c), n in cnt.most_common())))
    w('- %d entradas da referência primária ficaram de fora (não encontradas, só wrapper, '
      'confiança baixa ou repetidas):' % len(res['surface_dropped']))
    w('')
    for a, n, how, det in res['surface_dropped'][:40]:
        w('  - `%08X` %s: %s %s' % (a, n, HOW_PT.get(how, how), det))
    if len(res['surface_dropped']) > 40:
        w('  - ... e mais %d' % (len(res['surface_dropped']) - 40))
    w('')
    w('## Cena (DLSS / DLAA / DLSS 5)')
    w('')
    sc = res['scene']
    w('- Copiado da referência, **por medir neste jogo**: superfície `0x%X` (máscara `0x%X`), '
      'largura %s, pitch %s, altura %s.' % (sc.get('kSceneSurface', 0), sc.get('kSceneSurfaceMask', 0),
                                            sc.get('width'), sc.get('scaled_pitch'), sc.get('height')))
    w('- Para medir: correr o jogo com `--pgr4_native=true --pgr4_native_resolve_dump=<pasta>`, '
      'criar `<pasta>\\dump.request` numa cena 3D, e `python -m rexnative scene <pasta>\\f<N>-resolves.jsonl`.')
    check.append('Cena: medir a superfície da cena (python -m rexnative scene)')
    dv = res['devices']
    if not dv.get('follow_swap_device_only'):
        check.append('Devices: o renderer segue qualquer device (kFollowSwapDeviceOnly false). Se o '
                     'jogo desenhar noutro device ao carregar (corrupção do heap 0xC0000374, como '
                     'Iron Brigade), pôr "devices": {"follow_swap_device_only": true} no overrides.json')
    w('')
    w('## A verificar')
    w('')
    for c in check:
        w('- %s' % c)
    w('')
    return '\n'.join(L)


def _cmp(a, b):
    return 'igual' if a == b else '**diferente**'


def validation_report(res, hand, ref_note=''):
    """Compares the adapter's result for a port with the port's hand-made profile."""
    L = []
    w = L.append
    t = res['target']
    w('# Validação: %s gerado vs feito à mão' % t['title'])
    w('')
    if ref_note:
        w(ref_note)
        w('')
    w('Referências: %s (primária: %s).' % (', '.join(r['name'] for r in res['references']),
                                          res['primary']))
    w('')
    stats = collections.Counter()
    w('## Funções')
    w('')
    w('| Função | Gerado | À mão | | Como (gerado) |')
    w('|---|---|---|---|---|')
    for canon in prof.FUNCTIONS:
        g = res['functions'][canon]['address']
        h = hand['functions'].get(canon)
        if canon == 'ResolveInner' and h is None and g == res['functions']['Resolve']['address']:
            g = None
        verdict = 'igual' if g == h else ('**só gerado**' if g and not h else
                                          ('**só à mão**' if h and not g else '**diferente**'))
        stats['functions ' + verdict.strip('*')] += 1
        r = res['functions'][canon]
        w('| %s | %s | %s | %s | %s %s |' % (canon, _h(g), _h(h), verdict,
                                            HOW_PT.get(r['how'], r['how']),
                                            CONF_PT.get(r['confidence'], '')))
    w('')
    w('## Resolve')
    w('')
    ra = res.get('resolve_abi') or {}
    w('- Argumentos (posição pública r3..r10 ← registo de entrada, 0 = constante): gerado `%s`, '
      'à mão `%s` — %s.' % (ra.get('args'), hand['resolve_abi']['args'],
                            _cmp(ra.get('args'), hand['resolve_abi']['args'])))
    cc, hc = res.get('clear_color'), hand.get('clear_color')
    w('- Cor de clear por defeito: gerado %s, à mão %s — %s.' % (
        ('`%08X` %s' % (cc['address'], 'vector' if cc['vector'] else 'escalar')) if cc else '—',
        ('`%08X` %s' % (hc['address'], 'vector' if hc['vector'] else 'escalar')) if hc else '—',
        _cmp(cc and (cc['address'], cc['vector']), hc and (hc['address'], hc['vector']))))
    w('')
    w('## Offsets do device')
    w('')
    diff_offs = [(k, v['target'], hand['offsets'].get(k), v['how']) for k, v in res['offsets'].items()
                 if v['target'] != hand['offsets'].get(k)]
    same = len(res['offsets']) - len(diff_offs)
    w('- %d de %d iguais.' % (same, len(res['offsets'])))
    for k, g, h, how in diff_offs:
        w('- **%s**: gerado %s, à mão %s (%s)' % (k, g, h, how))
    stats['offsets equal'] = same
    stats['offsets different'] = len(diff_offs)
    w('')
    w('## d3d_surface.inc (por nome)')
    w('')
    gen = {n: a for a, n, c, how, conf, src in res['surface']}
    hm = {n: a for a, n, c in hand['surface']}
    agree = [n for n in gen if hm.get(n) == gen[n]]
    differ = [n for n in gen if n in hm and hm[n] != gen[n]]
    only_g = [n for n in gen if n not in hm]
    only_h = [n for n in hm if n not in gen]
    w('- iguais %d, diferentes %d, só no gerado %d, só à mão %d.' % (len(agree), len(differ),
                                                                     len(only_g), len(only_h)))
    stats.update({'surface equal': len(agree), 'surface different': len(differ),
                  'surface only generated': len(only_g), 'surface only hand-made': len(only_h)})
    for n in differ:
        w('  - diferente: %s gerado `%08X` à mão `%08X`' % (n, gen[n], hm[n]))
    for n in only_h:
        w('  - só à mão: %s `%08X`' % (n, hm[n]))
    if only_g:
        w('  - só no gerado: %s' % ', '.join('%s `%08X`' % (n, gen[n]) for n in only_g))
    w('')
    w('## Diff das camadas (mesmo formato, sem comentários)')
    w('')
    a = emit.game_layer_h(emit.from_profile(hand), comments=False).splitlines()
    b = emit.game_layer_h(emit.from_result(res), comments=False).splitlines()
    d = list(difflib.unified_diff(a, b, 'à mão', 'gerado', lineterm='', n=0))
    w('```diff')
    L.extend(d or ['(sem diferenças)'])
    w('```')
    w('')
    return '\n'.join(L), stats
