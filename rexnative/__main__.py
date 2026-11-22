"""ReXNative: retargets the PGR4Native D3D12 renderer's game layer to other
ReXGlue-recompiled Xbox 360 games.

  python -m rexnative info XEX...
  python -m rexnative extract [NAME...]                   reference profiles -> references/
  python -m rexnative adapt --generated DIR --xex FILE --name NAME --out DIR [--refs a,b]
  python -m rexnative validate NAME [--refs a,b] [--out DIR]
  python -m rexnative scene RESOLVES.jsonl
  python -m rexnative neutral [--port NAME] [--out DIR]   game-neutral src/native (renderer/)
  python -m rexnative prepare SOURCE... --work DIR        a game as it comes -> codegen, layer
  python -m rexnative project WORK/GAME [--build]         a prepared game -> a project that runs
  python -m rexnative title-update WORK...                the title updates XboxUnity lists
  python -m rexnative dlc WORK/GAME SOURCE...             install downloadable content
  python -m rexnative dlc WORK... [--list]                its DLC from archive.org's collection
  python -m rexnative deck WORK/GAME... [--steam] [--try S]  copy built games to the Steam Deck
  python -m rexnative art WORK/GAME CAPTURE.png [--title T]   Steam artwork for its shortcut
  python -m rexnative survey --work DIR                   table of the prepared games

Reports and messages are in English; --lang pt (before or after the command) or
REXNATIVE_LANG=pt writes them in Portuguese.
"""
import argparse
import json
import os
import sys

from . import adapt as adaptmod, emit, i18n, profile as prof, report, scene as scenemod, xex
from .i18n import tr

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
    return {n: prof.as_reference(prof.extract(ports[n], n)) for n in names}


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


def cmd_refs_pack(a):
    """Each port's profile and listings in <out>/<port>/, and <out>/ports.json naming them as
    packs: copied to another machine, it is that machine's ports.json (--ports)."""
    ports = prof.load_ports(a.ports)
    os.makedirs(a.out, exist_ok=True)
    listed = {'_comment': 'Reference packs (rexnative refs-pack): profiles and listings derived '
                          'from the games. For this machine only, never for publishing.'}
    for n in a.names or ports:
        folder = os.path.join(a.out, n)
        p = prof.save_pack(ports[n], n, folder)
        listed[n] = {'title': p.get('title', n), 'pack': n}
        size = sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder))
        print('%s: %d functions found, %.1f MB -> %s' % (
            n, sum(1 for v in p['functions'].values() if v), size / 1e6, folder))
    with open(os.path.join(a.out, 'ports.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(listed, f, indent=1)
        f.write('\n')


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
        res, hand, tr('v_note', port['root']))
    _write(os.path.join(out, 'validation.md'), text)
    with open(os.path.join(out, 'validation.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(dict(stats), f, indent=1)
    print('validation: ' + ', '.join('%s %d' % kv for kv in sorted(stats.items())))
    print('wrote %s' % os.path.join(out, 'validation.md'))


def cmd_scene(a):
    s = scenemod.find(scenemod.read(a.jsonl))
    if not s:
        sys.exit(tr('s_none', a.jsonl))
    print(scenemod.describe(s))


def cmd_neutral(a):
    from . import neutral
    if os.path.isdir(os.path.join(a.out, 'src', 'native')) and not a.force:
        sys.exit("%s already has src/native (the repository's renderer, which may have changes "
                 "of its own): pass --force to replace it with the port's, or --out elsewhere"
                 % a.out)
    ports = prof.load_ports(a.ports)
    neutral.build(ports[a.port], a.port, prof.extract(ports[a.port], a.port), a.out, ports)


def cmd_prepare(a):
    from . import prepare
    if os.name == 'nt':  # the matching and the codegen it starts stay behind the user's work
        import ctypes
        k32 = ctypes.windll.kernel32
        k32.SetPriorityClass(k32.GetCurrentProcess(), prepare.BELOW_NORMAL)
    rexglue = a.rexglue or os.environ.get('REXNATIVE_REXGLUE')
    if not rexglue or not os.path.exists(rexglue):
        sys.exit('rexglue.exe not found: pass --rexglue or set REXNATIVE_REXGLUE '
                 '(scripts/build-sdk.ps1 installs it in <sdk-install>/bin)')
    ports = prof.load_ports(a.ports)
    sources = []
    work = os.path.normcase(os.path.abspath(a.work))
    for s in a.sources:
        names = sorted(os.listdir(s)) if os.path.isdir(s) else []
        top_xex = any(n.lower() == 'default.xex' for n in names)
        if os.path.isdir(s) and not top_xex:
            # A folder of games: each archive, package or sub-folder in it (never the work
            # folder, which may sit inside it).
            for n in names:
                p = os.path.join(s, n)
                if os.path.normcase(os.path.abspath(p)) == work:
                    continue
                if (n.lower().endswith(prepare.ARCHIVES + prepare.IMAGES) or os.path.isdir(p) or
                        prepare.stfs.is_package(p)):
                    sources.append(p)
        else:
            sources.append(s)
    refs = _refs(ports, a.refs.split(',') if a.refs else None)
    failed = []
    import time
    for s in sources:
        # Where this game's time goes (prepare.SPENT), said in its last line: the log has no clock.
        prepare.SPENT.clear()
        t0 = time.time()
        st = prepare.prepare(s, a.work, rexglue, refs, run_adapt, max_rounds=a.max_rounds,
                             title_update=a.title_update, dlc=a.dlc)
        if a.project and st.get('ok'):
            # One game's failed build or run does not stop the others: it is reported at the end.
            try:
                _project_chain(a, s, os.path.join(a.work, st['name']), rexglue, refs)
            except SystemExit as e:
                print('%s: %s' % (st['name'], e), file=sys.stderr)
                failed.append(st['name'])
        parts = prepare.spent_text()
        print('%s: %.0f s%s' % (st['name'], time.time() - t0, ' (%s)' % parts if parts else ''))
    text = prepare.survey(a.work)
    with open(os.path.join(a.work, 'survey.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(text)
    if failed:
        sys.exit('not built or not run: ' + ', '.join(failed))


def _project_chain(a, source, work, rexglue, refs):
    """prepare --project: a prepared game to a built one that runs. The project is written and
    built; the function starts, jump tables, setjmp / longjmp and XAPI fibers are found in the
    loaded image; the layer is made again over the functions codegen now knows (a layer made
    right after the first codegen misses D3D functions whose starts it did not know yet), between
    that codegen run and its build, so one build compiles the new code and the new layer (it was
    a build of its own, after it); then --discover runs."""
    from . import prepare, project
    args = (a.sdk_install, a.sdk_src, a.fidelityfx, a.dlss_sdk, a.dlss_nr)
    layer = os.path.join(work, 'layer', 'game_layer.h')
    done = os.path.join(work, 'codegen.ok')

    def text(p):
        return open(p, encoding='utf-8').read() if os.path.exists(p) else ''

    def stamp(p):
        return os.path.getmtime(p) if os.path.exists(p) else None

    made = []

    def layer_again():
        prepare.prepare(source, a.work, rexglue, refs, run_adapt, max_rounds=a.max_rounds)
        made.append(stamp(done))

    project.write(work)
    project.build_fixing(work, rexglue, args, jobs=a.jobs)
    project.find_functions(work, rexglue, args, before_build=layer_again)
    # Nothing found (no codegen run, so no layer made again), or the build after it had codegen
    # run once more (branch targets in other functions): the layer is made again here, and the
    # game built again if it changed.
    if not made or made[-1] != stamp(done):
        before = text(layer)
        prepare.prepare(source, a.work, rexglue, refs, run_adapt, max_rounds=a.max_rounds)
        if text(layer) != before:
            print('%s: the layer changed over the functions found: built again' % os.path.basename(work))
            project.build_fixing(work, rexglue, args, jobs=a.jobs)
    if a.discover:
        project.discover(work, rexglue, args, rounds=a.discover, seconds=a.discover_seconds)


def cmd_project(a):
    from . import project
    name = project.write(a.work)
    print('%s: project written in %s' % (name, os.path.abspath(a.work)))
    args = (a.sdk_install, a.sdk_src, a.fidelityfx, a.dlss_sdk, a.dlss_nr)
    rexglue = a.rexglue or os.environ.get('REXNATIVE_REXGLUE')
    if rexglue and not os.path.exists(rexglue):
        rexglue = None
    if (a.discover or a.find_functions) and not rexglue:
        sys.exit('rexglue.exe not found: pass --rexglue or set REXNATIVE_REXGLUE')
    if a.build or a.discover or a.find_functions:
        if rexglue:  # branch targets in other functions become hints, codegen runs again
            project.build_fixing(a.work, rexglue, args, jobs=a.jobs)
        else:
            project.build(a.work, *args, jobs=a.jobs)
    if a.find_functions:
        added = project.find_functions(a.work, rexglue, args)
        print('%d function starts added to hints.txt' % len(added))
    if a.discover:
        added = project.discover(a.work, rexglue, args, rounds=a.discover,
                                 seconds=a.discover_seconds)
        print('%d functions added to hints.txt' % len(added))


def cmd_deck(a):
    from . import deck
    d = deck.LocalDeck() if a.local else deck.Deck(a.host, a.key)
    root = deck.REMOTE_ROOT
    if a.root:
        root = deck.sd_root(d) if a.root.lower() == 'sd' else a.root.rstrip('/')
    games = [g for g in (deck.install(d, w, root=root, reinstall=a.reinstall) for w in a.work)
             if g]
    if not games:
        print('nothing sent')
        return
    if a.steam:
        added = deck.add_to_steam(d, games)
        if added is None:
            sys.exit('the Deck stayed in use: no shortcut added, no game tried')
        if not added:
            # The games whose shortcut was added are still tried (try_game looks each one up).
            print('some shortcuts were not added')
    if a.try_seconds:
        rows = []
        for g in games:
            r = deck.try_game(d, g, a.try_seconds)
            if r:
                rows.append('| %s | %s | %s | %s | %s |' % (
                    r['title'], 'yes' if r['running'] else 'NO',
                    ('%.1f ms (%.0f FPS), %.0f draws' % (r['frame_ms'], 1000.0 / r['frame_ms'],
                                                         r['draws'])) if r['frame_ms'] else '-',
                    '; '.join(f[-120:] for f in r['fatal']) or '%d errors' % r['errors'],
                    ', '.join(os.path.basename(c) for c in r['captures']) or '-'))
            else:
                rows.append('| %s | not tried | - | - | - |' % g['title'])
        report = os.path.join(os.path.dirname(games[0]['work']), 'deck-report.md')
        with open(report, 'w', encoding='utf-8', newline='\n') as f:
            f.write('| Game | Running | Frame | Errors | Captures |\n|---|---|---|---|---|\n' +
                    '\n'.join(rows) + '\n')
        print(open(report, encoding='utf-8').read())


def cmd_art(a):
    from . import art
    art.make(os.path.abspath(a.work), a.capture, a.title)


def cmd_title_update(a):
    from . import title_update as tu, xex
    works = []
    for w in a.work:
        if os.path.exists(tu.game_xex(w)):
            works.append(w)
        elif os.path.isdir(w):
            works += [os.path.join(w, n) for n in sorted(os.listdir(w))
                      if os.path.exists(tu.game_xex(os.path.join(w, n)))]
    for w in works:
        info = xex.read(tu.game_xex(w))
        try:
            ups = [u for u in tu.available(info['title_id'], info['media_id'])
                   if not u['base_version'] or u['base_version'] == info['version']]
        except OSError as e:
            print('%s: XboxUnity did not answer (%s)' % (os.path.basename(w), e))
            continue
        cur = tu.installed(w)
        print('%s: %s media %s version %s; installed: %s; listed: %s' % (
            os.path.basename(w), info['title_id'], info['media_id'], info['version'],
            ('TU %d' % cur['title_update']) if cur else 'none',
            ', '.join('TU %d (%d KB)' % (u['version'], u['size']) for u in ups[:6]) or 'none'))


def cmd_dlc(a):
    from . import dlc, title_update as tu
    if a.sources:
        names = dlc.install(a.work, a.sources)
        print('%d package(s) installed in %s' % (len(names), os.path.join(a.work, 'run', 'userdata')))
        return
    # No packages: the game's (or each prepared game's) DLC from archive.org's collection.
    if os.path.exists(tu.game_xex(a.work)):
        works = [a.work]
    else:
        works = [os.path.join(a.work, n) for n in sorted(os.listdir(a.work))
                 if os.path.exists(tu.game_xex(os.path.join(a.work, n)))]
    failed = []
    for w in works:
        print('== %s' % os.path.basename(w))
        try:
            got = dlc.fetch(w, list_only=a.list, keep=a.keep)
        except dlc.NotLoggedIn:
            raise
        except Exception as e:  # one game's failure does not stop the others
            print('  FAILED: %s' % e)
            failed.append(os.path.basename(w))
            continue
        n = sum(len(v.get('installed', [])) for v in got.values())
        if got and not a.list:
            print('  %d package(s) of %d archive(s)' % (n, len(got)))
    if failed:
        sys.exit('DLC not fetched: ' + ', '.join(failed))


def cmd_survey(a):
    from . import prepare
    print(prepare.survey(a.work))


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8')
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(prog='rexnative', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ports', default=PORTS, help='known ports (default: ports.json)')
    ap.add_argument('--lang', choices=i18n.LANGS, default=None,
                    help='language of reports and messages (default: $REXNATIVE_LANG or en)')
    # The same option after the command (python -m rexnative validate x --lang pt).
    lang_opt = argparse.ArgumentParser(add_help=False)
    lang_opt.add_argument('--lang', choices=i18n.LANGS, default=argparse.SUPPRESS,
                          help='language of reports and messages (en, pt)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('info', help='XEX title, version and XDK libraries', parents=[lang_opt])
    s.add_argument('xex', nargs='+')
    s.set_defaults(fn=cmd_info)
    s = sub.add_parser('extract', help='write the hand-made ports\' profiles', parents=[lang_opt])
    s.add_argument('names', nargs='*')
    s.add_argument('--out', default=os.path.join(HERE, 'references'))
    s.set_defaults(fn=cmd_extract)
    s = sub.add_parser('refs-pack', help='reference packs of the known ports, for a machine '
                       'without their sources or generated code (the Steam Deck)', parents=[lang_opt])
    s.add_argument('out', help='folder: one sub-folder per port and a ports.json naming them')
    s.add_argument('names', nargs='*')
    s.set_defaults(fn=cmd_refs_pack)
    s = sub.add_parser('adapt', help='generate a game layer for a new game', parents=[lang_opt])
    s.add_argument('--generated', required=True, help='the game\'s ReXGlue generated/default')
    s.add_argument('--xex', required=True, help='its default.xex')
    s.add_argument('--name', required=True)
    s.add_argument('--title')
    s.add_argument('--refs', help='references to use (default: every port in ports.json)')
    s.add_argument('--out', required=True)
    s.set_defaults(fn=cmd_adapt)
    s = sub.add_parser('validate', help='regenerate a known port and compare with its hand-made layer', parents=[lang_opt])
    s.add_argument('name')
    s.add_argument('--refs')
    s.add_argument('--out')
    s.set_defaults(fn=cmd_validate)
    s = sub.add_parser('scene', help='propose the scene surface from a resolve dump', parents=[lang_opt])
    s.add_argument('jsonl')
    s.set_defaults(fn=cmd_scene)
    s = sub.add_parser('neutral', help='write the game-neutral renderer sources', parents=[lang_opt])
    s.add_argument('--port', default='ironbrigade', help='port whose src/native is the newest')
    s.add_argument('--out', default=os.path.join(HERE, 'renderer'))
    s.add_argument('--force', action='store_true', help='replace an existing src/native')
    s.set_defaults(fn=cmd_neutral)
    s = sub.add_parser('prepare', parents=[lang_opt],
                       help='a game as it comes (archive, STFS package, folder; or a folder of '
                            'them) -> extracted, ReXGlue codegen, its layer')
    s.add_argument('sources', nargs='+')
    s.add_argument('--work', required=True, help='one folder per game is made here')
    s.add_argument('--rexglue', help='rexglue.exe (default: $REXNATIVE_REXGLUE)')
    s.add_argument('--refs', help='references to use (default: every port in ports.json)')
    s.add_argument('--max-rounds', type=int, default=40, help='codegen rounds adding hints')
    s.add_argument('--title-update', action='store_true',
                   help='download the newest title update XboxUnity lists for each game and '
                        'install it before codegen (the code is recompiled from the patched '
                        'image); see `rexnative title-update`')
    s.add_argument('--dlc', action='store_true',
                   help="download each game's DLC from archive.org's Xbox 360 DLC collection "
                        '(msx360gcdlc; an archive.org login, see `rexnative dlc`) and install it')
    s.add_argument('--project', action='store_true',
                   help='then make each game a built project that runs, as `rexnative project '
                        '--build --find-functions`, the layer made again over the functions found, '
                        'and --discover N')
    s.add_argument('--discover', type=int, default=12, metavar='N',
                   help='with --project: runs adding the functions the game reports missing')
    s.add_argument('--discover-seconds', type=int, default=45)
    s.add_argument('--jobs', type=int, default=0)
    penv = os.environ.get
    s.add_argument('--sdk-install', default=penv('REXNATIVE_SDK_INSTALL'))
    s.add_argument('--sdk-src', default=penv('REXNATIVE_SDK_SRC'))
    s.add_argument('--fidelityfx', default=penv('REXNATIVE_FIDELITYFX'))
    s.add_argument('--dlss-sdk', default=penv('REXNATIVE_DLSS_SDK'))
    s.add_argument('--dlss-nr', default=penv('REXNATIVE_DLSS_NR'))
    s.set_defaults(fn=cmd_prepare)
    s = sub.add_parser('title-update', parents=[lang_opt],
                       help="the title updates XboxUnity lists for games in prepare's --work, "
                            'and the one installed (nothing is downloaded)')
    s.add_argument('work', nargs='+', help="a game's folder, or prepare's --work")
    s.set_defaults(fn=cmd_title_update)
    s = sub.add_parser('dlc', parents=[lang_opt],
                       help="install a game's downloadable content (STFS packages, or folders "
                            'and archives holding them) where the game finds it; without them, '
                            "fetch it from archive.org's Xbox 360 DLC collection (msx360gcdlc)")
    s.add_argument('work', help="the game's folder in prepare's --work (without sources: or "
                                'the --work folder, every game in it)')
    s.add_argument('sources', nargs='*', help='packages, folders or .rar/.7z/.zip archives')
    s.add_argument('--list', action='store_true',
                   help="the collection's archives for each game, nothing downloaded")
    s.add_argument('--keep', action='store_true',
                   help='keep the downloaded archives in <work>/<game>/dlc-download')
    s.set_defaults(fn=cmd_dlc)
    s = sub.add_parser('deck', parents=[lang_opt],
                       help='copy built games to the Steam Deck, add them to Steam, try them')
    s.add_argument('work', nargs='+', help="a game's folder in prepare's --work (built)")
    s.add_argument('--host', help='deck@ADDRESS[:PORT] (or REXNATIVE_DECK)')
    s.add_argument('--key', help='SSH key (or REXNATIVE_DECK_KEY)')
    s.add_argument('--local', action='store_true',
                   help='this machine is the Deck (the tool runs on it): no SSH; the game\'s '
                        'files are hard links to the work folder\'s where they can be')
    s.add_argument('--root', metavar='FOLDER|sd',
                   help="the Deck's folder for the games (absolute, or under the deck user's "
                        "home; default Games/ReXNative); sd: ReXNative/ on the microSD card")
    s.add_argument('--steam', action='store_true',
                   help="add the games to Steam's library (closes Steam; Game Mode restarts it)")
    s.add_argument('--reinstall', action='store_true',
                   help="send games their user removed from the Deck too (<root>/.removed/<slug>; "
                        "without it they are left out)")
    s.add_argument('--try', dest='try_seconds', type=int, default=0, metavar='SECONDS',
                   help='start each game from Steam, read its log and take a capture after '
                        'SECONDS, then close it')
    s.set_defaults(fn=cmd_deck)
    s = sub.add_parser('art', parents=[lang_opt],
                       help="Steam artwork for a game's Deck shortcut (<work>/deck/art), from one "
                            'of its captures (best its title screen); needs Pillow')
    s.add_argument('work', help="the game's folder in prepare's --work")
    s.add_argument('capture', help='a capture of the game (live.ps1 frame or shot, deck/try-*.png)')
    s.add_argument('--title', help="the name under the picture (default: the archive's)")
    s.set_defaults(fn=cmd_art)
    s = sub.add_parser('project', parents=[lang_opt],
                       help='a game `prepare` made -> a ReXGlue project with ReXNative that runs '
                            '(--build: built into build/, laid out in run/)')
    s.add_argument('work', help="the game's folder in prepare's --work")
    s.add_argument('--build', action='store_true')
    env = os.environ.get
    s.add_argument('--sdk-install', default=env('REXNATIVE_SDK_INSTALL'))
    s.add_argument('--sdk-src', default=env('REXNATIVE_SDK_SRC'))
    s.add_argument('--fidelityfx', default=env('REXNATIVE_FIDELITYFX'))
    s.add_argument('--dlss-sdk', default=env('REXNATIVE_DLSS_SDK'))
    s.add_argument('--dlss-nr', default=env('REXNATIVE_DLSS_NR'))
    s.add_argument('--jobs', type=int, default=0)
    s.add_argument('--find-functions', action='store_true',
                   help='after the build, dump the loaded image and find the function starts '
                        'codegen missed (reached only through pointers) ahead, as hints; then '
                        'codegen and the build run again')
    s.add_argument('--discover', type=int, default=0, metavar='N',
                   help='after the build, run the game up to N times: each function it reports '
                        'missing (a thread entry, an indirect call) becomes a codegen hint, then '
                        'codegen and the build run again')
    s.add_argument('--discover-seconds', type=int, default=40,
                   help='a run that lasts this long without a missing function ends --discover')
    s.add_argument('--rexglue', help='rexglue.exe, for --discover (default: $REXNATIVE_REXGLUE)')
    s.set_defaults(fn=cmd_project)
    s = sub.add_parser('survey', parents=[lang_opt], help='table of the games prepared in --work')
    s.add_argument('--work', required=True)
    s.set_defaults(fn=cmd_survey)
    a = ap.parse_args(argv)
    i18n.set_lang(a.lang)
    a.fn(a)


if __name__ == '__main__':
    main()
