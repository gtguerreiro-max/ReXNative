"""A game's own code hooks: fixes that belong to one executable, as ReXGlue's [[midasm_hook]]s
(a host function called at a guest address with the registers named) and the C++ that defines
them.

rexnative/title_hooks/<title id>-<module hash>/ holds them for the executable whose Xenia canary
module hash (fps_boost.module_hash: the code as loaded, title update applied) is that one:
- hooks.toml, the [[midasm_hook]] entries, written to <work>/title_hooks.toml (the manifest
  includes it, prepare._write_manifest);
- hooks.cpp, written to <work>/src/title_hooks.cpp (the game's CMakeLists builds it in when
  there; it may call the renderer's RexNativeTitleSwaps and RexNativeSetTitleSwapHook).
Another executable (another title update, another region's build) gets neither: they are taken
out again. The hash is known after a first build (ReXGlue logs it): a game's first build has no
hooks, the next one does.
"""
import os
import shutil

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'title_hooks')
TOML = 'title_hooks.toml'
SOURCE = os.path.join('src', 'title_hooks.cpp')


def folder(work):
    """The hooks' folder for the game's executable, or None."""
    from . import fps_boost
    try:
        tid = fps_boost._title_id(work)
        h = fps_boost.module_hash(work)
    except (OSError, ValueError):
        return None
    if not tid or not h:
        return None
    d = os.path.join(HERE, '%s-%s' % (tid.upper(), h.upper()))
    return d if os.path.isfile(os.path.join(d, 'hooks.toml')) else None


def _same(a, b):
    try:
        with open(a, 'rb') as fa, open(b, 'rb') as fb:
            return fa.read() == fb.read()
    except OSError:
        return False


def apply(work, log=print):
    """Writes the game's hooks into its work folder, or takes out hooks of another executable.
    The hooks' folder is logged.
    Returns whether its hooks changed (codegen then runs again: the hooks are in the code)."""
    d = folder(work)
    changed = False
    pairs = [(os.path.join(d, 'hooks.toml') if d else None, os.path.join(work, TOML)),
             (os.path.join(d, 'hooks.cpp') if d else None, os.path.join(work, SOURCE))]
    for src, dst in pairs:
        if src and os.path.exists(src):
            if not _same(src, dst):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
                changed = True
        elif os.path.exists(dst):
            os.remove(dst)
            changed = True
            log('title hooks: %s taken out (not this executable\'s)' % os.path.relpath(dst, work))
    if d:
        log('title hooks: %s' % os.path.basename(d))
    return changed
