"""Compiles ReXNative's renderer (renderer/src) against a game layer with a port's build flags:
syntax only (clang-cl /Zs), nothing is written.

  python -m rexnative.checkneutral --build GAME_BUILD_DIR --hooks HOOKS.cpp [--drop DIR] LAYER_DIR [FILE...]

GAME_BUILD_DIR is a configured ReXGlue game build (its build.ninja; read only), HOOKS.cpp the
name of a source file it compiles with the renderer's flags (its D3D hooks file), --drop an
include directory of that build to leave out (the port's own src/, so its copies of the renderer
are not found first). Without FILEs: the hooks file, the HUD and every renderer/src/native/*.cpp.
"""
import argparse
import glob
import os
import sys

from . import compilecheck

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv):
    ap = argparse.ArgumentParser(prog='rexnative.checkneutral')
    ap.add_argument('--build', required=True)
    ap.add_argument('--hooks', required=True)
    ap.add_argument('--drop', action='append', default=[])
    ap.add_argument('layer')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args(argv)
    layer = os.path.abspath(a.layer)
    src = os.path.join(HERE, 'renderer', 'src')
    files = a.files or ([os.path.join(src, 'rexnative_d3d_hooks.cpp'),
                         os.path.join(src, 'hud', 'hud.cpp'),
                         os.path.join(src, 'glue_defaults.cpp')] +
                        sorted(glob.glob(os.path.join(src, 'native', '*.cpp'))))
    print('layer %s: %d files' % (layer, len(files)))
    res = compilecheck.check(a.build, a.hooks, files, [layer, src], drop_includes=a.drop)
    bad = [f for f, (rc, e, o) in res.items() if rc]
    for f in bad:
        print('---', f)
        print(res[f][2][-3000:])
    print('%d of %d compiled' % (len(files) - len(bad), len(files)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
