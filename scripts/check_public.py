"""Checks that nothing private is about to be published: run it before a push to this public
repository (`python scripts/check_public.py`, or as a pre-push hook: `--hook` installs one).

It looks through every tracked file (at HEAD, at the revision given, or on disk with
`--worktree`) for what must not leave the machine: home network addresses, a Windows user's own
folder, a PC's default name, e-mail addresses other than GitHub's noreply ones, and the patterns
in a list kept outside the repository, so that the list itself stays private:
$REXNATIVE_PUBLIC_DENY, else ~/.config/rexnative/public-deny.txt (one regular expression a line,
# for comments; a name, a drive's folder, a private source). As a pre-push hook it also checks
the commits being pushed: their authors' and committers' e-mails and their messages. Exit code
1, with each line found, when something is there.

Third-party addresses are not private: the copyright holders in THIRD_PARTY_NOTICES.md and the
upstream authors on an SDK patch's From: line are left alone.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMAIL = (r'[A-Za-z0-9._%+-]+@(?!users\.noreply\.github\.com|anthropic\.com|example\.)'
         r'[A-Za-z0-9.-]+\.(?!invalid\b)[a-z]{2,}')
GENERIC = [
    r'\b192\.168\.(?!1\.10\b)\d{1,3}\.\d{1,3}\b',  # a home network (the docs' example is 192.168.1.10)
    r'\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b',
    r'DESKTOP-[A-Z0-9]{6,}',  # a PC's default name
    r'[A-Za-z]:\\+Users\\+(?!<|Public\\|Default\\|%)[A-Za-z0-9._-]+',  # a user's own folder
    r'/home/(?!deck/|<|\$)[a-z0-9._-]+',
]
# Binary files are not looked into.
SKIP_EXT = ('.png', '.jpg', '.jpeg', '.ico', '.dll', '.exe', '.lib', '.bin', '.dxbc', '.cso', '.zip')
THIRD_PARTY = ('THIRD_PARTY_NOTICES.md',)
# A commit's author and committer: GitHub's noreply address, or a co-author's.
COMMIT_EMAIL_OK = re.compile(r'@(users\.noreply\.github\.com|anthropic\.com)$', re.I)


def deny_patterns():
    path = os.environ.get('REXNATIVE_PUBLIC_DENY') or os.path.join(
        os.path.expanduser('~'), '.config', 'rexnative', 'public-deny.txt')
    extra = []
    if os.path.exists(path):
        for n, line in enumerate(open(path, encoding='utf-8'), 1):
            line = line.strip()
            if line and not line.startswith('#'):
                try:
                    re.compile(line)
                except re.error as e:
                    sys.exit('check_public: %s:%d is not a regular expression (%s); '
                             'a backslash is written \\\\' % (path, n, e))
                extra.append(line)
    return GENERIC + extra, path if extra else None


def git(*args, check=True):
    return subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True,
                          check=check).stdout


def files_at(rev):
    if rev is None:  # the working tree: tracked files and new ones not ignored
        out = git('ls-files', '--cached', '--others', '--exclude-standard')
    else:
        out = git('ls-tree', '-r', '--name-only', rev)
    return [f for f in out.decode('utf-8').splitlines()
            if f and not f.lower().endswith(SKIP_EXT)]


def read(rev, f):
    if rev is None:
        try:
            with open(os.path.join(ROOT, f), 'rb') as fh:
                return fh.read()
        except OSError:  # deleted but not yet staged
            return b''
    return git('show', '%s:%s' % (rev, f), check=False)


def scan(text, where, private, email, hits):
    for i, line in enumerate(text.splitlines(), 1):
        m = private.search(line)
        if not m and email.search(line):
            m = email.search(line)
        if m:
            hits.append('%s:%d: %s' % (where, i, m.group(0)))


def compile_all():
    patterns, private_list = deny_patterns()
    private = re.compile('|'.join('(?:%s)' % p for p in patterns), re.I)
    return private, re.compile(EMAIL, re.I), private_list


def check(rev='HEAD'):
    private, email, private_list = compile_all()
    never = re.compile(r'(?!)')
    hits = []
    for f in files_at(rev):
        text = read(rev, f).decode('utf-8', 'replace')
        if f in THIRD_PARTY:
            scan(text, f, private, never, hits)
            continue
        if f.endswith('.patch'):
            # The upstream author's From: header is public; the rest of the patch is ours.
            text = '\n'.join('' if ln.startswith('From: ') else ln for ln in text.splitlines())
        scan(text, f, private, email, hits)
    return hits, private_list


def check_commits(spec):
    """The commits `spec` (git rev-list arguments) names: their e-mails and messages."""
    private, email, _ = compile_all()
    hits = []
    out = git('log', '--format=%H%x00%ae%x00%ce%x00%B%x01', *spec).decode('utf-8', 'replace')
    for rec in out.split('\x01'):
        rec = rec.strip('\n')
        if not rec:
            continue
        sha, ae, ce, body = rec.split('\x00', 3)
        for who, addr in (('author', ae), ('committer', ce)):
            if not COMMIT_EMAIL_OK.search(addr):
                hits.append('commit %s: %s e-mail is not a noreply address' % (sha[:10], who))
        scan(body, 'commit %s message' % sha[:10], private, email, hits)
    return hits


HOOK = """#!/bin/sh
# Installed by scripts/check_public.py --hook: nothing private goes to the public repository.
exec python "$(git rev-parse --show-toplevel)/scripts/check_public.py" --pre-push
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('rev', nargs='?', default='HEAD')
    ap.add_argument('--worktree', action='store_true',
                    help='check the files on disk instead of a revision')
    ap.add_argument('--hook', action='store_true', help='install the pre-push hook')
    ap.add_argument('--pre-push', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.hook:
        hooks = git('rev-parse', '--git-path', 'hooks').decode().strip()
        hooks = os.path.join(ROOT, hooks) if not os.path.isabs(hooks) else hooks
        os.makedirs(hooks, exist_ok=True)
        with open(os.path.join(hooks, 'pre-push'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(HOOK)
        print('pre-push hook installed in %s' % hooks)
        return 0
    found, private_list = [], None
    if a.pre_push:
        # git passes "<local ref> <local sha> <remote ref> <remote sha>" lines on stdin.
        zero = '0' * 40
        for ln in sys.stdin:
            parts = ln.split()
            if len(parts) != 4 or parts[1] == zero:
                continue  # a deletion
            local, remote = parts[1], parts[3]
            hits, private_list = check(local)
            found += hits
            spec = [local, '--not', '--remotes'] if remote == zero else ['%s..%s' % (remote, local)]
            found += check_commits(spec)
    else:
        found, private_list = check(None if a.worktree else a.rev)
    if found:
        print('Not published: private data (scripts/check_public.py):')
        for h in found:
            print('  ' + h)
        return 1
    print('check_public: nothing private found%s' % (
        ' (with the private list %s)' % private_list if private_list else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
