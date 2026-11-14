"""ReXNative: retargets the PGR4Native D3D12 renderer's game layer to other
ReXGlue-recompiled Xbox 360 games.

  python -m rexnative info XEX...
  python -m rexnative extract [NAME...]                   reference profiles -> references/
  python -m rexnative adapt --generated DIR --xex FILE --name NAME --out DIR [--refs a,b]
  python -m rexnative validate NAME [--refs a,b] [--out DIR]
  python -m rexnative scene RESOLVES.jsonl
  python -m rexnative neutral [--port NAME] [--out DIR]   game-neutral src/native (renderer/)
"""
import argparse
import json
import os
import sys

from . import adapt as adaptmod, emit, profile as prof, report, scene as scenemod, xex

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# ports.json: this machine's ports (paths), not in the repository; ports.example.json shows the
# format.
PORTS = os.path.join(HERE, 'ports.json')
if not os.path.exists(PORTS):
    PORTS = os.path.join(HERE, 'ports.example.json')


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def _refs(ports, names, exclude=()):
    names = [n for n in (names or ports) if n not in exclude]
    return {n: prof.extract(ports[n], n) for n in names}


def cmd_info(a):
    for path in a.xex:
        i = xex.read(path)
        lib, ver = xex.d3d_library(i)
        print('%s: title %s version %s (%s) D3D %s %s' % (path, i['title_id'], i['version'],
                                                         i['pe_name'], lib, ver))
        print('  ' + ', '.join('%s %s' % kv for kv in i['libs'].items()))


def cmd_extract(a):
    ports = prof.load_ports(a.ports)
    os.makedirs(a.out, exist_ok=True)
    for n in a.names or ports:
        p = prof.extract(ports[n], n)
        prof.save(p, os.path.join(a.out, n + '.json'))
        print('%s: %d functions, %d surface entries, %d offsets -> %s' % (
            n, sum(1 for v in p['functions'].values() if v), len(p['surface']), len(p['offsets']),
            os.path.join(a.out, n + '.json')))


def run_adapt(target, refs, out, log=print):
    res = adaptmod.adapt(target, refs, adaptmod.load_overrides(out), log=log)
    layer = emit.from_result(res)
    emit.write(layer, out, res)
    _write(os.path.join(out, 'report.md'), report.adapter_report(res))
    log('wrote %s (game_layer.h, d3d_surface.inc, layer.json, report.md)' % out)
    return res


def cmd_adapt(a):
    ports = prof.load_ports(a.ports)
    refs = _refs(ports, a.refs.split(',') if a.refs else None, exclude=[a.name])
    target = {'name': a.name, 'title': a.title or a.name, 'generated': a.generated, 'xex': a.xex}
    run_adapt(target, refs, a.out)


def cmd_validate(a):
    ports = prof.load_ports(a.ports)
    port = ports[a.name]
    hand = prof.extract(port, a.name)
    refs = _refs(ports, a.refs.split(',') if a.refs else None, exclude=[a.name])
    tag = ('from-' + '-'.join(refs)) if a.refs else 'auto'
    out = a.out or os.path.join(HERE, 'results', '%s-%s' % (a.name, tag))
    p = prof.port_paths(port)
    target = {'name': a.name, 'title': port.get('title', a.name), 'generated': p['generated'],
              'xex': p['xex']}
    res = run_adapt(target, refs, out)
    hand_layer = emit.from_profile(hand)
    _write(os.path.join(out, 'hand_made', 'game_layer.h'), emit.game_layer_h(hand_layer))
    _write(os.path.join(out, 'hand_made', 'd3d_surface.inc'), emit.surface_inc(hand_layer))
    text, stats = report.validation_report(
        res, hand, 'O port feito à mão está em `%s`; a camada dele, escrita no mesmo formato, em '
                   '`hand_made/`.' % port['root'])
    _write(os.path.join(out, 'validation.md'), text)
    with open(os.path.join(out, 'validation.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(dict(stats), f, indent=1)
    print('validation: ' + ', '.join('%s %d' % kv for kv in sorted(stats.items())))
    print('wrote %s' % os.path.join(out, 'validation.md'))


def cmd_scene(a):
    s = scenemod.find(scenemod.read(a.jsonl))
    if not s:
        sys.exit('no colour resolve in %s' % a.jsonl)
    print(scenemod.describe(s))


def cmd_neutral(a):
    from . import neutral
    if os.path.isdir(os.path.join(a.out, 'src', 'native')) and not a.force:
        sys.exit("%s already has src/native (the repository's renderer, which may have changes "
                 "of its own): pass --force to replace it with the port's, or --out elsewhere"
                 % a.out)
    ports = prof.load_ports(a.ports)
    neutral.build(ports[a.port], a.port, prof.extract(ports[a.port], a.port), a.out, ports)


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8')
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(prog='rexnative', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ports', default=PORTS, help='known ports (default: ports.json)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('info', help='XEX title, version and XDK libraries')
    s.add_argument('xex', nargs='+')
    s.set_defaults(fn=cmd_info)
    s = sub.add_parser('extract', help='write the hand-made ports\' profiles')
    s.add_argument('names', nargs='*')
    s.add_argument('--out', default=os.path.join(HERE, 'references'))
    s.set_defaults(fn=cmd_extract)
    s = sub.add_parser('adapt', help='generate a game layer for a new game')
    s.add_argument('--generated', required=True, help='the game\'s ReXGlue generated/default')
    s.add_argument('--xex', required=True, help='its default.xex')
    s.add_argument('--name', required=True)
    s.add_argument('--title')
    s.add_argument('--refs', help='references to use (default: every port in ports.json)')
    s.add_argument('--out', required=True)
    s.set_defaults(fn=cmd_adapt)
    s = sub.add_parser('validate', help='regenerate a known port and compare with its hand-made layer')
    s.add_argument('name')
    s.add_argument('--refs')
    s.add_argument('--out')
    s.set_defaults(fn=cmd_validate)
    s = sub.add_parser('scene', help='propose the scene surface from a resolve dump')
    s.add_argument('jsonl')
    s.set_defaults(fn=cmd_scene)
    s = sub.add_parser('neutral', help='write the game-neutral renderer sources')
    s.add_argument('--port', default='ironbrigade', help='port whose src/native is the newest')
    s.add_argument('--out', default=os.path.join(HERE, 'renderer'))
    s.add_argument('--force', action='store_true', help='replace an existing src/native')
    s.set_defaults(fn=cmd_neutral)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == '__main__':
    main()
