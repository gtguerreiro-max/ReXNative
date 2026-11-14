"""Syntax-only compile (clang-cl /Zs) of renderer sources with a port's own build flags.

Reads the compiler, defines, flags and include directories of a port's hooks file from its
build.ninja (read only), puts the given include directories first (the neutral sources and a
game layer) and compiles each file with /Zs: nothing is written.
"""
import os
import re
import shlex
import subprocess


def ninja_flags(build_dir, source_name):
    ninja = open(os.path.join(build_dir, 'build.ninja'), encoding='utf-8', errors='replace').read()
    rules = open(os.path.join(build_dir, 'CMakeFiles', 'rules.ninja'), encoding='utf-8',
                 errors='replace').read()
    m = re.search(r'^build [^\n]*%s\.obj: (\S+) [^\n]*\n((?:  [^\n]*\n)+)' % re.escape(source_name),
                  ninja, re.M)
    if not m:
        raise SystemExit('no build line for %s in %s' % (source_name, build_dir))
    rule, body = m.group(1), m.group(2)
    vars_ = dict(re.findall(r'^  (\w+) = (.*)$', body, re.M))
    rm = re.search(r'^rule %s\n(?:  [^\n]*\n)*?  command = (.*)$' % re.escape(rule), rules, re.M)
    compiler = re.search(r'"([^"]+clang-cl\.exe)"', rm.group(1)).group(1)
    def split(s):
        return shlex.split(s.replace('$:', ':'), posix=False)
    return {
        'compiler': compiler,
        'defines': [d.replace('\\"', '"') for d in split(vars_.get('DEFINES', ''))],
        'flags': split(vars_.get('FLAGS', '')),
        'includes': split(vars_.get('INCLUDES', '')),
    }


def check(build_dir, source_name, files, first_includes, drop_includes=(), log=print):
    f = ninja_flags(build_dir, source_name)
    drop = {os.path.normcase(os.path.normpath(d)) for d in drop_includes}
    includes = ['-I' + d for d in first_includes]
    for inc in f['includes']:
        path = inc[2:] if inc.startswith('-I') else None
        if path and os.path.normcase(os.path.normpath(path)) in drop:
            continue
        includes.append(inc)
    flags = [x for x in f['flags'] if x not in ('/O2', '/Ob2')]
    results = {}
    for src in files:
        cmd = [f['compiler'], '/nologo', '-TP'] + f['defines'] + includes + flags + ['/Zs', src]
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace',
                           creationflags=0x00004000 if os.name == 'nt' else 0)  # below normal
        out = (p.stdout + p.stderr).strip()
        errors = [l for l in out.splitlines() if ' error' in l or 'error:' in l]
        results[src] = (p.returncode, errors, out)
        log('  %s %s%s' % ('ok  ' if p.returncode == 0 else 'FAIL', os.path.basename(src),
                           (': ' + errors[0]) if errors else ''))
    return results
