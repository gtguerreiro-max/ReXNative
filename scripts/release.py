"""Releases ReXNative: `python scripts/release.py 0.1.0 [--notes FILE] [--dry-run] [--final]`.

A release is a published commit of main that was tested: it gets the tag vX.Y.Z, the `stable`
branch moves to it (what the ReXNative Manager builds with on its stable channel; main is its dev
channel), and a GitHub release says what changed since the previous one: the --notes file, else
the commit subjects since the previous tag (the first release needs --notes). While ReXNative is
in Early Access the releases are pre-releases; --final makes one the latest release.

Nothing is done when: the tree has uncommitted changes, HEAD is not on origin/main, check_public.py
finds something private, the tag exists, or origin's stable is not behind HEAD (it only moves
forward). --dry-run says what would be done.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDENTITY = ('gtguerreiro-max', '295622337+gtguerreiro-max@users.noreply.github.com')
sys.path.insert(0, os.path.join(ROOT, 'scripts'))


def git(*args, check=True):
    r = subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if check and r.returncode != 0:
        sys.exit('git %s: %s' % (' '.join(args), (r.stderr or r.stdout).strip()))
    return r


def repo():
    url = git('remote', 'get-url', 'origin').stdout.strip()
    m = re.search(r'github\.com[/:]([^/]+/[^/]+?)(?:\.git)?/?$', url)
    if not m:
        sys.exit('origin is not a GitHub repository: %s' % url)
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('version', help='X.Y.Z')
    ap.add_argument('--notes', help='a Markdown file with the release text')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--final', action='store_true', help='a release, not a pre-release (Early Access)')
    a = ap.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+', a.version):
        sys.exit('the version is X.Y.Z: %s' % a.version)
    tag = 'v' + a.version
    git('fetch', '-q', '--tags', 'origin')
    head = git('rev-parse', 'HEAD').stdout.strip()
    if git('status', '--porcelain', '--untracked-files=no').stdout.strip():
        sys.exit('uncommitted changes: a release is a published commit')
    if git('merge-base', '--is-ancestor', 'HEAD', 'origin/main', check=False).returncode != 0:
        sys.exit('HEAD (%s) is not on origin/main: push it first' % head[:10])
    import check_public
    hits, _ = check_public.check('HEAD')
    if hits:
        sys.exit('check_public.py found private data:\n  ' + '\n  '.join(hits[:20]))
    if git('rev-parse', '-q', '--verify', 'refs/tags/' + tag, check=False).returncode == 0:
        sys.exit('%s exists already' % tag)
    stable = git('rev-parse', '-q', '--verify', 'refs/remotes/origin/stable', check=False).stdout.strip()
    if stable and git('merge-base', '--is-ancestor', stable, 'HEAD', check=False).returncode != 0:
        sys.exit('origin/stable (%s) is not behind HEAD: it only moves forward' % stable[:10])
    prev = git('describe', '--tags', '--abbrev=0', '--match', 'v[0-9]*', 'HEAD', check=False).stdout.strip()
    if a.notes:
        notes = open(a.notes, encoding='utf-8').read()
    elif prev:
        log = git('log', '--no-merges', '--format=- %s', '%s..HEAD' % prev).stdout.strip()
        notes = 'Changes since %s:\n\n%s\n' % (prev, log)
    else:
        sys.exit('the first release needs --notes (no earlier tag to list the changes from)')
    if not a.final:
        notes = ('**Early Access**: ReXNative builds and runs the games in its compatibility list '
                 '(docs/GAMES.md), but it is still being tested; expect rough edges and report them '
                 'in the issues.\n\n' + notes)
    title = 'ReXNative %s%s' % (a.version, '' if a.final else ' (Early Access)')
    print('%s at %s (%s)%s; stable %s -> %s; GitHub release "%s" on %s' % (
        tag, head[:10], git('log', '-1', '--format=%s').stdout.strip()[:60],
        ', after %s' % prev if prev else ', the first', stable[:10] or '(new)', head[:10], title, repo()))
    if a.dry_run:
        print('--- notes ---\n' + notes)
        return 0
    git('-c', 'user.name=%s' % IDENTITY[0], '-c', 'user.email=%s' % IDENTITY[1],
        'tag', '-a', tag, '-m', title)
    git('push', '-q', 'origin', 'refs/tags/%s' % tag, 'HEAD:refs/heads/stable')
    with tempfile.NamedTemporaryFile('w', suffix='.md', delete=False, encoding='utf-8') as f:
        f.write(notes)
    try:
        subprocess.run(['gh', 'release', 'create', tag, '-R', repo(), '--verify-tag', '--title', title,
                        '--notes-file', f.name] + (['--latest'] if a.final else ['--prerelease']),
                       check=True)
    finally:
        os.remove(f.name)
    print('released %s: https://github.com/%s/releases/tag/%s' % (tag, repo(), tag))
    return 0


if __name__ == '__main__':
    sys.exit(main())
