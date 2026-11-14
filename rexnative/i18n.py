"""The language of the tool's reports and messages: English (default) or Portuguese (pt-PT).

  python -m rexnative --lang pt ...   or   REXNATIVE_LANG=pt

tr(key, *args) formats the message `key` of the current language with `%` (English when the
language lacks it). Generated C++ (game_layer.h, d3d_surface.inc) stays in English.
"""
import os

LANGS = ('en', 'pt')
_lang = os.environ.get('REXNATIVE_LANG', 'en').lower()[:2]
if _lang not in LANGS:
    _lang = 'en'


def set_lang(lang):
    global _lang
    if lang:
        lang = lang.lower()[:2]
        if lang not in LANGS:
            raise SystemExit('rexnative: unknown language %r (en, pt)' % lang)
        _lang = lang


def lang():
    return _lang


def tr(key, *args):
    text = MESSAGES[_lang].get(key)
    if text is None:
        text = MESSAGES['en'][key]
    return text % args if args else text


def table(name):
    """A lookup table (confidences, match methods) of the current language."""
    return TABLES[_lang].get(name) or TABLES['en'][name]


TABLES = {
    'en': {
        'conf': {'high': 'high', 'medium': 'medium', 'low': 'low', 'override': 'manual', None: '—'},
        'how': {
            'exact': 'same listing', 'masked': 'same listing (numbers masked)',
            'region': 'same, the only one in the library', 'callgraph': 'called at the same place',
            'neighbour': 'next to a matched one', 'callees': 'calls the same functions',
            'fuzzy': 'similar listing', 'wrapper': 'only the inner function exists',
            'public wrapper': 'public wrapper of the matched function',
            'abi': "Resolve's inner function", 'same as Resolve': 'the same as Resolve',
            'not found': 'not found', 'override': 'overrides.json',
        },
    },
    'pt': {
        'conf': {'high': 'alta', 'medium': 'média', 'low': 'baixa', 'override': 'manual', None: '—'},
        'how': {
            'exact': 'listagem igual', 'masked': 'listagem igual (números mascarados)',
            'region': 'igual, única na biblioteca', 'callgraph': 'chamada no mesmo sítio',
            'neighbour': 'vizinha de uma já encontrada', 'callees': 'chama as mesmas funções',
            'fuzzy': 'listagem parecida', 'wrapper': 'só existe a função interna',
            'public wrapper': 'wrapper público da função encontrada',
            'abi': 'função interna do Resolve', 'same as Resolve': 'a mesma do Resolve',
            'not found': 'não encontrada', 'override': 'overrides.json',
        },
    },
}

MESSAGES = {
    'en': {
        # adapt: why a reference was chosen
        'why_same_xdk': 'same XDK version (%s)',
        'why_xdk': 'XDK %s vs %s',
        'why_both': 'both %s',
        'why_ltcg': 'LTCG',
        'why_no_ltcg': 'without LTCG',
        'why_libs': '%s vs %s',
        'why_no_xex': 'no xex',
        # adapter report
        'r_title': '# Generated layer: %s',
        'r_exe': '- Executable: title id `%s`, version `%s`, `%s`, D3D library **%s**',
        'r_generated': '- Generated code: `%s` (%d functions)',
        'r_library': '- D3D library found at: %s',
        'r_refs': '## References',
        'r_refs_head': '| Reference | Score | Why | Found |',
        'r_primary': ' (primary)',
        'r_of': '%d of %d',
        'r_funcs': '## Functions the renderer uses',
        'r_funcs_head': '| Function | Address | How | Confidence | Detail |',
        'r_agrees': 'agrees',
        'r_disagrees': '**disagrees**',
        'c_disagree': '%s: %s says `%08X`, the primary `%s`',
        'c_medium': '%s `%08X`: medium confidence (%s)',
        'c_not_found': '%s: not found%s',
        'r_hook': '- Hook at `%08X`%s; ABI: **%s**.',
        'r_inner': ' (inner function `%08X`)',
        'r_public': 'public',
        'r_remapped': 'remapped',
        'r_args_head': '| Public argument | Public position | At the hook\'s entry | Votes |',
        'r_assumed': 'assumed',
        'c_overruled': "Resolve: the reference's layer sets %s to 0, but the register is used as an "
                       'argument in both listings: the register is used',
        'c_assumed': 'Resolve: no aligned use for %s (assumed at the public position)',
        'c_stack': 'Resolve: stack arguments elsewhere: the generic EntryArgs does not remap them',
        'r_no_hook': "- Could not decide Resolve's hook.",
        'c_no_hook': 'Resolve: hook to be decided',
        'r_clear': '## Default clear colour (pClearColor null)',
        'r_vector': 'a 16-byte vector (`lvx128`)',
        'r_scalar': 'one float repeated 4 times (`lfs`)',
        'r_clear_found': '- `%08X`, %s.',
        'r_not_found': '- Not found.',
        'c_no_clear': 'Default clear colour not found',
        'r_ref_inconsistent': "- **The reference (%s) is inconsistent**: the port says `%s` (%s), but "
                              "the reference's own listing uses `%s` (%s).",
        'w_vector': 'vector',
        'w_scalar': 'scalar',
        'r_offsets': '## Device offsets',
        'r_offsets_head': '| Name | Reference | Target | Votes | Note |',
        'c_split': 'offset %s: split vote',
        'r_entries': '- %d entries: %s.',
        'r_dropped': "- %d of the primary reference's entries were left out (not found, wrapper only, "
                     'low confidence or repeated):',
        'r_more': '  - ... and %d more',
        'r_scene': '## Scene (DLSS / DLAA / DLSS 5)',
        'r_scene_copied': "- Copied from the reference, **to be measured in this game**: surface `0x%X` "
                          '(mask `0x%X`), width %s, pitch %s, height %s.',
        'r_scene_how': '- To measure it: run the game with `--pgr4_native=true '
                       '--pgr4_native_resolve_dump=<dir>`, create `<dir>\\dump.request` in a 3D scene, '
                       'and `python -m rexnative scene <dir>\\f<N>-resolves.jsonl`.',
        'c_scene': "Scene: measure the scene surface (python -m rexnative scene)",
        'c_devices': 'Devices: the renderer follows any device (kFollowSwapDeviceOnly false). If the '
                     'game draws on another device while loading (heap corruption 0xC0000374, as '
                     'Iron Brigade), put "devices": {"follow_swap_device_only": true} in overrides.json',
        'r_check': '## To check',
        # validation report
        'v_title': '# Validation: %s generated vs hand-made',
        'v_refs': 'References: %s (primary: %s).',
        'v_funcs': '## Functions',
        'v_funcs_head': '| Function | Generated | Hand-made | | How (generated) |',
        'v_equal': 'equal',
        'v_only_gen': '**generated only**',
        'v_only_hand': '**hand-made only**',
        'v_different': '**different**',
        'v_args': '- Arguments (public position r3..r10 ← entry register, 0 = constant): generated '
                  '`%s`, hand-made `%s` — %s.',
        'v_clear': '- Default clear colour: generated %s, hand-made %s — %s.',
        'v_offsets_same': '- %d of %d equal.',
        'v_offset_diff': '- **%s**: generated %s, hand-made %s (%s)',
        'v_surface': '## d3d_surface.inc (by name)',
        'v_surface_counts': '- equal %d, different %d, generated only %d, hand-made only %d.',
        'v_surface_diff': '  - different: %s generated `%08X` hand-made `%08X`',
        'v_surface_hand': '  - hand-made only: %s `%08X`',
        'v_surface_gen': '  - generated only: %s',
        'v_diff': '## Layer diff (same format, no comments)',
        'v_diff_hand': 'hand-made',
        'v_diff_gen': 'generated',
        'v_no_diff': '(no differences)',
        'v_note': 'The hand-made port is in `%s`; its layer, written in the same format, in '
                  '`hand_made/`.',
        # scene
        's_scene': 'Scene: resolve #%d, %dx%d, pitch %d, MSAA %s, format %s%s.',
        's_hdr': 'DLSS 5: the scene resolve is HDR, before tone mapping: '
                 'pgr4_native_dlss5_stage = "scene".',
        's_ldr': 'DLSS 5: the scene is not in an HDR format in this resolve (tone mapping already '
                 'done, or an LDR scene): find the HDR pass with RenderDoc, or use '
                 'pgr4_native_dlss5_stage = "front".',
        's_later': 'Full-frame resolves after the scene (post-processing / UI): %s.',
        's_none': 'no colour resolve in %s',
        # neutral (renderer/CHANGES.md)
        'n_title': '# renderer/: the neutral src/native',
        'n_made': 'Made by `python -m rexnative neutral --port %s` from that port\'s `src/native` '
                  'and `%s`.',
        'n_done': '## Edits applied (%d)',
        'n_failed': '## Edits that failed (%d)',
        'n_none_f': '- none',
        'n_left': '## Guest addresses left in code',
        'n_none': '- none',
    },
    'pt': {
        'why_same_xdk': 'mesma versão do XDK (%s)',
        'why_xdk': 'XDK %s vs %s',
        'why_both': 'ambos %s',
        'why_ltcg': 'LTCG',
        'why_no_ltcg': 'sem LTCG',
        'why_libs': '%s vs %s',
        'why_no_xex': 'sem xex',
        'r_title': '# Camada gerada: %s',
        'r_exe': '- Executável: title id `%s`, versão `%s`, `%s`, biblioteca D3D **%s**',
        'r_generated': '- Código gerado: `%s` (%d funções)',
        'r_library': '- Biblioteca D3D encontrada em: %s',
        'r_refs': '## Referências',
        'r_refs_head': '| Referência | Pontuação | Porquê | Encontradas |',
        'r_primary': ' (primária)',
        'r_of': '%d de %d',
        'r_funcs': '## Funções que o renderer usa',
        'r_funcs_head': '| Função | Endereço | Como | Confiança | Detalhe |',
        'r_agrees': 'concorda',
        'r_disagrees': '**discorda**',
        'c_disagree': '%s: %s diz `%08X`, a primária `%s`',
        'c_medium': '%s `%08X`: confiança média (%s)',
        'c_not_found': '%s: não encontrada%s',
        'r_hook': '- Hook em `%08X`%s; ABI: **%s**.',
        'r_inner': ' (função interna `%08X`)',
        'r_public': 'pública',
        'r_remapped': 'remapeada',
        'r_args_head': '| Argumento público | Posição pública | Na entrada do hook | Votos |',
        'r_assumed': 'assumido',
        'c_overruled': 'Resolve: a camada da referência põe %s a 0, mas o registo é usado como '
                       'argumento nas duas listagens: usado o registo',
        'c_assumed': 'Resolve: sem uso alinhado para %s (assumido na posição pública)',
        'c_stack': 'Resolve: argumentos de pilha noutro sítio: o EntryArgs genérico não os remapeia',
        'r_no_hook': '- Não foi possível decidir o hook do Resolve.',
        'c_no_hook': 'Resolve: hook por decidir',
        'r_clear': '## Cor de clear por defeito (pClearColor nulo)',
        'r_vector': 'vector de 16 bytes (`lvx128`)',
        'r_scalar': 'um float repetido 4 vezes (`lfs`)',
        'r_clear_found': '- `%08X`, %s.',
        'r_not_found': '- Não encontrada.',
        'c_no_clear': 'Cor de clear por defeito não encontrada',
        'r_ref_inconsistent': '- **A referência (%s) está inconsistente**: o port diz `%s` (%s), mas a '
                              'listagem da própria referência usa `%s` (%s).',
        'w_vector': 'vector',
        'w_scalar': 'escalar',
        'r_offsets': '## Offsets do device',
        'r_offsets_head': '| Nome | Referência | Alvo | Votos | Nota |',
        'c_split': 'offset %s: votos divididos',
        'r_entries': '- %d entradas: %s.',
        'r_dropped': '- %d entradas da referência primária ficaram de fora (não encontradas, só '
                     'wrapper, confiança baixa ou repetidas):',
        'r_more': '  - ... e mais %d',
        'r_scene': '## Cena (DLSS / DLAA / DLSS 5)',
        'r_scene_copied': '- Copiado da referência, **por medir neste jogo**: superfície `0x%X` '
                          '(máscara `0x%X`), largura %s, pitch %s, altura %s.',
        'r_scene_how': '- Para medir: correr o jogo com `--pgr4_native=true '
                       '--pgr4_native_resolve_dump=<pasta>`, criar `<pasta>\\dump.request` numa cena '
                       '3D, e `python -m rexnative scene <pasta>\\f<N>-resolves.jsonl`.',
        'c_scene': 'Cena: medir a superfície da cena (python -m rexnative scene)',
        'c_devices': 'Devices: o renderer segue qualquer device (kFollowSwapDeviceOnly false). Se o '
                     'jogo desenhar noutro device ao carregar (corrupção do heap 0xC0000374, como '
                     'Iron Brigade), pôr "devices": {"follow_swap_device_only": true} no overrides.json',
        'r_check': '## A verificar',
        'v_title': '# Validação: %s gerado vs feito à mão',
        'v_refs': 'Referências: %s (primária: %s).',
        'v_funcs': '## Funções',
        'v_funcs_head': '| Função | Gerado | À mão | | Como (gerado) |',
        'v_equal': 'igual',
        'v_only_gen': '**só gerado**',
        'v_only_hand': '**só à mão**',
        'v_different': '**diferente**',
        'v_args': '- Argumentos (posição pública r3..r10 ← registo de entrada, 0 = constante): '
                  'gerado `%s`, à mão `%s` — %s.',
        'v_clear': '- Cor de clear por defeito: gerado %s, à mão %s — %s.',
        'v_offsets_same': '- %d de %d iguais.',
        'v_offset_diff': '- **%s**: gerado %s, à mão %s (%s)',
        'v_surface': '## d3d_surface.inc (por nome)',
        'v_surface_counts': '- iguais %d, diferentes %d, só no gerado %d, só à mão %d.',
        'v_surface_diff': '  - diferente: %s gerado `%08X` à mão `%08X`',
        'v_surface_hand': '  - só à mão: %s `%08X`',
        'v_surface_gen': '  - só no gerado: %s',
        'v_diff': '## Diff das camadas (mesmo formato, sem comentários)',
        'v_diff_hand': 'à mão',
        'v_diff_gen': 'gerado',
        'v_no_diff': '(sem diferenças)',
        'v_note': 'O port feito à mão está em `%s`; a camada dele, escrita no mesmo formato, em '
                  '`hand_made/`.',
        's_scene': 'Cena: resolve #%d, %dx%d, pitch %d, MSAA %s, formato %s%s.',
        's_hdr': 'DLSS 5: o resolve da cena é HDR, antes do tone mapping: '
                 'pgr4_native_dlss5_stage = "scene".',
        's_ldr': 'DLSS 5: a cena não está num formato HDR neste resolve (tone mapping já feito ou '
                 'cena LDR): confirmar com o RenderDoc qual o passe HDR, ou usar '
                 'pgr4_native_dlss5_stage = "front".',
        's_later': 'Resolves de ecrã inteiro depois da cena (pós-processamento / UI): %s.',
        's_none': 'nenhum resolve de cor em %s',
        'n_title': '# renderer/: o src/native neutro',
        'n_made': 'Gerado por `python -m rexnative neutral --port %s` a partir do `src/native` e do '
                  '`%s` desse port.',
        'n_done': '## Edições aplicadas (%d)',
        'n_failed': '## Edições que falharam (%d)',
        'n_none_f': '- nenhuma',
        'n_left': '## Endereços de guest que ficaram no código',
        'n_none': '- nenhum',
    },
}
