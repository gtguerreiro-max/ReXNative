"""Drafts a release's notes with a local model, for a person to read before scripts/release.py
publishes them: `python scripts/draft_release_notes.py [--since vX.Y.Z] [--out notes.md]`.

The script picks the material itself: the commits since the last release (subject and body, in
pieces that fit the model). The model gets a fixed task per piece: one note per change a user
would notice, each citing the commits it comes from, and the rest left out with a reason, as
JSON. The answer is then checked:
- every note cites commits of the range (an invented change cites nothing real);
- every commit is cited or left out (one that is neither is reported);
- nothing private is in the text (check_public.py's patterns, the private list included).
It writes the notes (`--out`) and a report beside them (`<out>.report.txt`), and never publishes.

The model is any OpenAI-compatible server: `--url` (default $REXNATIVE_LOCAL_LLM_URL, else
llama.cpp's llama-server at http://127.0.0.1:8081/v1) and `--model` (default inkling). It
costs no cloud usage: the reading is the model's, the checking this script's.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))

SECTIONS = ['Games', 'Renderer and settings', 'Steam Deck', 'Tool: prepare, codegen, build',
            'SDK patches', 'Developer tools']
# Characters of commit text per request: ~4 per token, well inside a 32k-token window with the
# instructions and the model's thinking.
PIECE = 40000
BODY = 1200  # a commit body's first characters (most say what changed in their first lines)

TASK = """You write the release notes of ReXNative, a native Direct3D 12 renderer and porting tool for Xbox 360 games recompiled with the ReXGlue SDK, which also runs them on the Steam Deck. The readers are players and people who port games.

Below are commits, each with its short hash, subject and body. Write the notes for them:
- One note per change a user would notice: a game that now runs or runs better, a new or changed setting, a fixed bug, a lighter or faster run, a new Steam Deck behaviour, a new tool command. Commits that are parts of one change make one note.
- A commit users never notice (a refactor, a test, wording in the docs, a script only developers run) goes in "left_out" with a short reason. Developer scripts may still get a note in the "Developer tools" section when they add something a contributor would use.
- Each note: one or two plain sentences in English, present tense, saying what now happens. Name the game, the setting (as the HUD or the docs name it) or the command. Keep numbers only when the commit states them. No marketing words, no exclamation marks, no emojis, no Markdown inside the text.
- Use only facts the commits state. Do not guess causes, numbers or games.
- "section" is one of: SECTIONS.
- "commits" lists the short hashes the note comes from, exactly as given.

Every commit below must appear either in some note's "commits" or in "left_out".

Answer with one JSON object and nothing else:
{"notes": [{"section": "...", "text": "...", "commits": ["abc1234"]}], "left_out": [{"commit": "abc1234", "reason": "..."}]}

Commits:
"""


EDIT = """Below are draft release notes of ReXNative (a renderer and porting tool for Xbox 360 games, run on Windows and the Steam Deck) as JSON. They are accurate but too long and too technical. Edit them into release notes players read:
- Notes about the same feature become one note, wherever they are (for example several commits about one setting, or a fix to something added in the same release), with all their commits.
- Each note: at most 35 words, one or two plain sentences saying what the user gets. Keep the game names, the settings as the menu names them, and at most one number when it shows the gain.
- Leave out internals: function and variable names, file paths, how something is implemented, thread or kernel details. A setting's key may stay when people type it in a file.
- Do not add facts that are not in the draft.
- "section" stays one of: SECTIONS. A note about the Steam Deck only goes in "Steam Deck".
- "commits" keeps the hashes exactly as given.

Answer with one JSON object and nothing else: {"notes": [{"section": "...", "text": "...", "commits": ["abc1234"]}]}

Draft:
"""


def git(*args):
    return subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, text=True,
                          encoding='utf-8', errors='replace', check=True).stdout


def commits(since, until):
    out = git('log', '--no-merges', '--reverse', '--format=%h%x1f%s%x1f%b%x1e', '%s..%s' % (since, until))
    res = []
    for rec in out.split('\x1e'):
        rec = rec.strip('\n')
        if not rec:
            continue
        h, s, b = (rec.split('\x1f') + ['', ''])[:3]
        body = '\n'.join(l for l in b.splitlines() if not l.startswith('Co-Authored-By:')).strip()
        res.append({'hash': h.strip(), 'subject': s.strip(), 'body': body[:BODY]})
    return res


def pieces(cs):
    out, cur, n = [], [], 0
    for c in cs:
        t = '[%s] %s\n%s\n' % (c['hash'], c['subject'], c['body'])
        if cur and n + len(t) > PIECE:
            out.append(cur)
            cur, n = [], 0
        cur.append((c, t))
        n += len(t)
    if cur:
        out.append(cur)
    return out


def ask(url, model, prompt, timeout, thinking=True):
    """The model's answer. Thinking helps it read the commits; the edit, plain rewriting, goes
    without (with it, the model spent its whole budget thinking and wrote nothing)."""
    # Templates take thinking as enable_thinking (Qwen's) or reasoning_effort (Inkling's: none to
    # max, 0.9 by default); each ignores the other's.
    body = {'model': model, 'messages': [{'role': 'user', 'content': prompt}],
            # Inkling thinks a little even at none: the edit of 19 notes needed more than 8192.
            'temperature': 0.6 if thinking else 0.3, 'top_p': 0.95, 'max_tokens': 32768,
            'chat_template_kwargs': {'enable_thinking': thinking,
                                     'reasoning_effort': 'high' if thinking else 'none'}}
    req = urllib.request.Request(url.rstrip('/') + '/chat/completions', data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.load(r)
    c = d['choices'][0]
    if not (c['message'].get('content') or '').strip() and c.get('finish_reason') == 'length':
        raise ValueError('the answer ran out of tokens before any text (thinking too long)')
    return c['message'].get('content') or ''


def parse(text):
    """The JSON object in the answer (bare, or in a ```json block)."""
    m = re.search(r'```(?:json)?\s*(\{.*\})\s*```', text, re.S)
    raw = m.group(1) if m else text[text.find('{'):text.rfind('}') + 1]
    d = json.loads(raw)
    if not isinstance(d.get('notes'), list):
        raise ValueError('no "notes" list')
    return d


def draft_piece(url, model, piece, log, timeout):
    prompt = TASK.replace('SECTIONS', ', '.join('"%s"' % s for s in SECTIONS)) + '\n'.join(t for _, t in piece)
    return answer(url, model, prompt, log, timeout)


def edit(url, model, notes, log, timeout):
    """The second pass: the notes merged by feature, short and plain. Commits only from the draft."""
    prompt = EDIT.replace('SECTIONS', ', '.join('"%s"' % s for s in SECTIONS)) + json.dumps(
        {'notes': notes}, ensure_ascii=False, indent=1)
    return answer(url, model, prompt, log, timeout, thinking=False)['notes']


def answer(url, model, prompt, log, timeout, thinking=True):
    last = None
    for attempt in range(3):
        t0 = time.time()
        try:
            text = ask(url, model, prompt if last is None else
                       prompt + '\n\nYour previous answer was not valid (%s). Answer with the JSON object only.' % last,
                       timeout, thinking)
            log('  model answered in %.0f s (%d characters)' % (time.time() - t0, len(text)))
            return parse(text)
        except (ValueError, json.JSONDecodeError) as e:
            last = str(e)[:200]
            log('  not valid JSON (%s): asked again' % last)
    raise RuntimeError('the model gave no valid answer in 3 tries: %s' % last)


def check(notes, left_out, cs):
    """[problems] that stop the draft, [warnings] for the reader."""
    known = {c['hash'] for c in cs}

    def real(h):
        return next((k for k in known if k.startswith(h) or h.startswith(k)), None) if len(h) >= 7 else None
    problems, warnings, cited = [], [], set()
    for n in notes:
        if n.get('section') not in SECTIONS:
            warnings.append('a note in an unknown section "%s" goes under Developer tools: %s' % (n.get('section'), n.get('text', '')[:80]))
            n['section'] = 'Developer tools'
        hs = [real(str(h)) for h in n.get('commits') or []]
        if not hs or None in hs:
            problems.append('a note cites no commit of the range (%s): %s' % (n.get('commits'), n.get('text', '')[:100]))
        cited.update(h for h in hs if h)
        n['commits'] = [h for h in hs if h]
    out = {real(str(x.get('commit'))) for x in left_out}
    for c in cs:
        if c['hash'] not in cited and c['hash'] not in out:
            warnings.append('%s is neither in a note nor left out: %s' % (c['hash'], c['subject']))
    import check_public
    private, email, _ = check_public.compile_all()
    for n in notes:
        if private.search(n['text']) or email.search(n['text']):
            problems.append('private data in a note: %s' % n['text'][:100])
    return problems, warnings


def render(notes, since):
    lines = ['Changes since %s.' % since, '']
    for s in SECTIONS:
        ns = [n for n in notes if n['section'] == s]
        if ns:
            lines += ['## %s' % s, '']
            lines += ['- %s (%s)' % (n['text'].strip(), ', '.join(n['commits'])) for n in ns]
            lines.append('')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--since', help='the previous release tag (default: the latest v* tag)')
    ap.add_argument('--until', default='HEAD')
    ap.add_argument('--out', default=os.path.join(ROOT, 'release-notes.md'))
    ap.add_argument('--url', default=os.environ.get('REXNATIVE_LOCAL_LLM_URL') or 'http://127.0.0.1:8081/v1')
    ap.add_argument('--model', default='inkling')
    ap.add_argument('--timeout', type=int, default=1800, help='seconds per model answer')
    ap.add_argument('--no-edit', action='store_true', help='skip the second pass (merged, short, plain notes)')
    ap.add_argument('--from-draft', help="an earlier run's <out>.draft.json: only the edit runs again")
    a = ap.parse_args()
    since = a.since or git('describe', '--tags', '--abbrev=0', '--match', 'v[0-9]*', a.until).strip()
    cs = commits(since, a.until)
    if not cs:
        sys.exit('no commits since %s' % since)
    report = []

    def log(s):
        print(s, flush=True)
        report.append(s)
    log('%d commits since %s, in %d piece(s); model %s at %s' % (len(cs), since, len(pieces(cs)), a.model, a.url))
    notes, left_out = [], []
    t0 = time.time()
    if a.from_draft:  # the first pass's answer from an earlier run: only the edit again
        d = json.load(open(a.from_draft, encoding='utf-8'))
        notes, left_out = d['notes'], d.get('left_out') or []
        log('draft from %s: %d notes' % (a.from_draft, len(notes)))
    else:
        for i, p in enumerate(pieces(cs), 1):
            log('piece %d: %d commits' % (i, len(p)))
            d = draft_piece(a.url, a.model, p, log, a.timeout)
            notes += [n for n in d['notes'] if isinstance(n, dict) and n.get('text')]
            left_out += [x for x in d.get('left_out') or [] if isinstance(x, dict)]
        with open(a.out + '.draft.json', 'w', encoding='utf-8', newline='\n') as f:
            json.dump({'notes': notes, 'left_out': left_out}, f, ensure_ascii=False, indent=1)
    if not a.no_edit:
        draft = [dict(n) for n in notes]
        check(draft, left_out, cs)  # the draft's hashes made full, for the comparison below
        log('editing %d notes' % len(notes))
        edited = [n for n in edit(a.url, a.model, notes, log, a.timeout) if isinstance(n, dict) and n.get('text')]
        before = {h for n in draft for h in n['commits']}
        after = set()
        for n in edited:
            after.update(str(h) for h in n.get('commits') or [])
        lost = [h for h in before if not any(h.startswith(x) or x.startswith(h) for x in after if len(x) >= 7)]
        if lost:
            log('the edit dropped commits %s: their draft notes are kept' % ', '.join(lost))
            edited += [n for n in draft if any(h in lost for h in n['commits'])]
        notes = edited
    problems, warnings = check(notes, left_out, cs)
    text = render(notes, since)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    log('%d notes, %d commits left out, %.0f s' % (len(notes), len(left_out), time.time() - t0))
    for x in left_out:
        log('left out %s: %s' % (x.get('commit'), x.get('reason')))
    for w in warnings:
        log('WARNING: ' + w)
    for p in problems:
        log('PROBLEM: ' + p)
    with open(a.out + '.report.txt', 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(report) + '\n')
    print('draft: %s (report: %s.report.txt)' % (a.out, a.out))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
