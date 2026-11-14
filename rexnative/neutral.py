"""The renderer sources made game-neutral: one src/native (and one hooks file) for every game,
with everything game-specific read from the game's generated game_layer.h.

Built from the newest port (Iron Brigade's copy: From Dust's renderer plus its fixes) by a
fixed list of edits. Each edit must apply exactly once, so a renderer change upstream that
moves one of these spots is reported instead of silently skipped. Addresses left in comments
are replaced by the functions' names; any guest address left in code is listed.

Output (renderer/):
  src/native/...            the renderer, game-neutral
  src/rexnative_d3d_hooks.cpp  the guest D3D hooks (was <game>_d3d_trace.cpp)
  CHANGES.md                what was changed and what to check
"""
import os
import re
import shutil

from . import profile as prof

G = 'rexnative::game::'
INCLUDE = '#include "game_layer.h"  // this game\'s layer (rexnative adapt): addresses, offsets'


class Editor:
    def __init__(self):
        self.problems = []
        self.done = []

    def sub(self, text, fname, pattern, repl, count=1, flags=re.M, what=''):
        new, n = re.subn(pattern, repl, text, flags=flags)
        if (count is not None and n != count) or n == 0:
            self.problems.append('%s: %s: expected %s edit(s), found %d' % (
                fname, what or pattern[:60], count if count is not None else '>0', n))
            return text
        self.done.append('%s: %s (%d)' % (fname, what or pattern[:60], n))
        return new

    def add_include(self, text, fname, after):
        return self.sub(text, fname, r'^(%s)$' % re.escape(after), r'\1\n' + INCLUDE,
                        what='include game_layer.h')


def _hook_args(t, e, f):
    for enum, canon in prof.HOOKFN.items():
        t = e.sub(t, f, r'^(  %s = )0x[0-9A-Fa-f]{8},' % enum, r'\g<1>%skHook%s,' % (G, canon),
                  what=enum)
    return e.add_include(t, f, '#include <cstdint>')


def _d3d_state_h(t, e, f, p):
    for k in p['offsets']:
        if k.startswith('dev.'):
            name = k[4:]
            t = e.sub(t, f, r'^(constexpr uint32_t %s = )\d+;' % name, r'\g<1>%sdev::%s;' % (G, name),
                      what='dev::' + name)
    return t


def _d3d_state_cpp(t, e, f):
    return e.sub(t, f, r'\{(\d{4,5}), 0x(2[0-9A-F]{3}),', r'{%sshadow::k\2, 0x\2,' % G, count=None,
                 what='register shadow table')


def _render_targets_h(t, e, f, p):
    for k in p['offsets']:
        if k.startswith('rt.'):
            name = k[3:]
            t = e.sub(t, f, r'^(constexpr uint32_t %s = )\d+;' % name, r'\g<1>%srt::%s;' % (G, name),
                      what='rt::' + name)
    t = e.sub(t, f, r'(static constexpr uint32_t kScaledPitch = )\d+;', r'\g<1>%skScenePitch;' % G,
              what='kScaledPitch')
    return e.add_include(t, f, '#include "d3d12_device.h"')


def _occlusion_cpp(t, e, f):
    t = e.sub(t, f, r'constexpr uint32_t kDevFenceNext = \d+, kDevFenceDone = \d+;',
              'constexpr uint32_t kDevFenceNext = %socc::kDevFenceNext, kDevFenceDone = '
              '%socc::kDevFenceDone;' % (G, G), what='fences')
    return e.add_include(t, f, '#include "occlusion.h"')


def _resolve_cpp(t, e, f):
    pat = (r'^(?:      //[^\n]*\n)*      (?:const float c = LoadFloat\(base, 0x[0-9A-Fa-f]{8}u\);\n'
           r'      for \(float& v : p\.clear_color\) v = c;|for \(int i = 0; i < 4; \+\+i\) '
           r'p\.clear_color\[i\] = LoadFloat\(base, 0x[0-9A-Fa-f]{8}u \+ 4 \* i\);)\n')
    repl = ('      // The D3D\'s default (game_layer.h): XDK 6132 splats one float, XDK 20353 copies a\n'
            '      // 16-byte vector.\n'
            '      for (int i = 0; i < 4; ++i)\n'
            '        p.clear_color[i] = LoadFloat(\n'
            '            base, %skResolveDefaultClearColor + (%skResolveDefaultClearIsVector ? 4u * i : 0u));\n'
            % (G, G))
    return e.sub(t, f, pat, repl, what='default clear colour')


def _guest_d3d_cpp(t, e, f, shader_load):
    t = e.sub(t, f, r'^REX_EXTERN\(sub_%08X\);' % shader_load,
              'REX_EXTERN(REXNATIVE_SUB(REXNATIVE_SHADER_LOAD));', what='ShaderLoad extern')
    t = e.sub(t, f, r'\bsub_%08X\(ctx, base\);' % shader_load,
              'REXNATIVE_SUB(REXNATIVE_SHADER_LOAD)(ctx, base);', what='ShaderLoad call')
    return e.add_include(t, f, '#include "guest_d3d.h"')


def _scene(files, e):
    f = 'upscaler.h'
    files[f] = e.sub(files[f], f, r'(kSceneSurfaceMask = )0x[0-9A-Fa-f]+;', r'\g<1>%skSceneSurfaceMask;' % G)
    files[f] = e.sub(files[f], f, r'(kSceneSurface = )0x[0-9A-Fa-f]+;', r'\g<1>%skSceneSurface;' % G)
    f = 'fidelityfx.h'
    files[f] = e.sub(files[f], f, r'(kFfxSceneWidth = )\d+;', r'\g<1>%skSceneWidth;' % G)
    f = 'frame_height.h'
    files[f] = e.sub(files[f], f, r'(kGameFrameHeight = )\d+;', r'\g<1>%skGameFrameHeight;' % G)
    files[f] = e.add_include(files[f], f, '#include <cstdint>')
    f = 'renderer.cpp'
    files[f] = e.sub(files[f], f, r'(REXCVAR_DEFINE_INT32\(pgr4_render_height, )\d+,',
                     r'\g<1>%skDefaultRenderHeight,' % G)
    files[f] = e.sub(files[f], f, r'(REXCVAR_DEFINE_BOOL\(pgr4_native_occlusion, )(?:true|false),',
                     r'\g<1>%skDefaultOcclusion,' % G)
    f = 'present.cpp'
    files[f] = e.sub(files[f], f, r'(REXCVAR_DEFINE_DOUBLE\(pgr4_native_display_aspect, )[^,]+,',
                     r'\g<1>%skDefaultDisplayAspect,' % G)


KFN_BLOCK = '''// Ids of the hooked functions in d3d_surface.inc, by their address in game_layer.h; kFnCount
// (no hook has it) when the game has none.
constexpr FnId FnIdOf(uint32_t addr) {
  if (addr)
    for (uint32_t i = 0; i < kFnCount; ++i)
      if (kFns[i].addr == addr) return FnId(i);
  return kFnCount;
}
namespace game = rexnative::game;
constexpr FnId kFnSwap = FnIdOf(game::kFnSwap), kFnResolve = FnIdOf(game::kFnResolve),
               kFnCreateDevice = FnIdOf(game::kFnCreateDevice),
               kFnRestoreSegment13392 = FnIdOf(game::kFnRestoreSegment13392),
               kFnRestoreSegment = FnIdOf(game::kFnRestoreSegment),
               kFnStartSegment = FnIdOf(game::kFnStartSegment),
               kFnSwitchSegment = FnIdOf(game::kFnSwitchSegment),
               kFnMakeSpace = FnIdOf(game::kFnMakeSpace),
               kFnBeginVertices = FnIdOf(game::kFnBeginVertices),
               kFnRestorePush = FnIdOf(game::kFnRestorePush),
               kFnDrawVerticesUP = FnIdOf(game::kFnDrawVerticesUP),
               kFnDrawVertices = FnIdOf(game::kFnDrawVertices),
               kFnDrawIndexed = FnIdOf(game::kFnDrawIndexedVertices),
               kFnClear = FnIdOf(game::kFnClear), kFnBeginTiling = FnIdOf(game::kFnBeginTiling),
               kFnEndTiling = FnIdOf(game::kFnEndTiling),
               kFnQueryIssue = FnIdOf(game::kFnQueryIssue),
               kFnQueryGetData = FnIdOf(game::kFnQueryGetData);

// The five draw builders, which every game draw goes through.
constexpr FnId kDrawCounterFns[] = {kFnBeginVertices, kFnDrawVertices, kFnDrawIndexed,
                                    FnIdOf(game::kFnDrawIndx2Inline),
                                    FnIdOf(game::kFnDrawIndx2Split)};
'''

ENTRY_ARGS = '''  if constexpr (rexnative::game::kResolveRemapArgs) {
    if (a.fn == pgr4::native::HookFn::kResolve) {
      // The hooked Resolve is not the public one (an LTCG build's inner function): where each
      // public argument is at its entry (game_layer.h kResolveArgRegs; 0 = constant 0). The
      // stack arguments are where the public call has them.
      for (int i = 0; i < 8; ++i) {
        const uint32_t reg = rexnative::game::kResolveArgRegs[i];
        a.r[i] = reg ? r[reg - 3] : 0;
      }
    }
  }
'''


def _trace(t, e, f):
    t = e.add_include(t, f, '#include "native/renderer.h"')
    t = e.sub(t, f, r'^// Entry points the dispatch handles by name.*?\n// The calls '
              r'rex_drawcount_api_counters', KFN_BLOCK.rstrip('\n') + '\n'
              'constexpr bool IsDrawCounterFn(FnId id) {\n'
              '  for (FnId d : kDrawCounterFns)\n'
              '    if (d == id) return true;\n'
              '  return false;\n'
              '}\n'
              '// The calls rex_drawcount_api_counters', flags=re.M | re.S, what='kFn ids')
    t = e.sub(t, f, r'std::atomic<uint64_t> g_calls\[kFnCount\];  // ',
              'std::atomic<uint64_t> g_calls[kFnCount + 1];  // (+1: kFnCount, functions the game '
              'lacks) ', what='g_calls')
    # Iron Brigade's port before 2026-11-14 rearranged the arguments by hand (an `if` block);
    # since then a comment says there is nothing to rearrange. Either becomes the generic remap.
    t = e.sub(t, f, r'^(?:  if \(a\.fn == pgr4::native::HookFn::kResolve\) \{\n.*?\n  \}\n|'
              r'  // Iron Brigade hooks the D3D\'s inner Resolve.*?slice > 0\.\n)',
              ENTRY_ARGS, flags=re.M | re.S, what='Resolve arguments')
    t = e.sub(t, f, r'^  // Iron Brigade: no CreateDevice hook, so the first device seen is configured here\.',
              '  // No CreateDevice hook (game_layer.h kFnCreateDevice 0): the first device seen is\n'
              '  // configured here.', what='configure at first device')
    t = e.sub(t, f, r'^    // Iron Brigade: only Swap\'s device is followed .*\n'
              r'    if \(r3 && r3 != t\.device && \(kId == kFnSwap \|\| !t\.device\)\) \[\[unlikely\]\]',
              '    // game_layer.h kFollowSwapDeviceOnly: only Swap\'s device is followed (Iron Brigade\n'
              '    // draws on a second device while levels load); otherwise every call\'s.\n'
              '    if (r3 && r3 != t.device &&\n'
              '        (rexnative::game::kFollowSwapDeviceOnly ? (kId == kFnSwap || !t.device)\n'
              '                                             : (t.device || rexnative::game::kConfigureAtFirstDevice)))\n'
              '        [[unlikely]]', what='device following')
    t = e.sub(t, f, r'^  if constexpr \(kId == kFnSwap\) \{\n    uint32_t none = 0;',
              '  if constexpr (rexnative::game::kFollowSwapDeviceOnly && kId == kFnSwap) {\n'
              '    uint32_t none = 0;', what='main device: Swap')
    t = e.sub(t, f, r'^  if constexpr \(nat::IsStateFlushingFn\(kAddr\) \|\| kClass != kNatNone\) \{\n'
              r'    const uint32_t main_device',
              '  if constexpr (rexnative::game::kFollowSwapDeviceOnly &&\n'
              '                (nat::IsStateFlushingFn(kAddr) || kClass != kNatNone)) {\n'
              '    const uint32_t main_device', what='main device: calls')
    t = e.sub(t, f, r'PGR4_LOG_FIRST\(4, INFO, "Iron Brigade: \{\}', 'PGR4_LOG_FIRST(4, INFO, "{}',
              what='log text')
    t = e.sub(t, f, r'static constexpr const char\* kNames\[\] = \{"draw_[0-9A-F]{8}", "draw_[0-9A-F]{8}", '
              r'"draw_[0-9A-F]{8}",\s*"draw_[0-9A-F]{8}", "draw_[0-9A-F]{8}", "aux_[0-9A-F]{8}",',
              'static constexpr const char* kNames[] = {"draw_BeginVertices", "draw_DrawVertices",\n'
              '                                           "draw_DrawIndexedVertices", "draw_DrawIndx2Inline",\n'
              '                                           "draw_DrawIndx2Split", "aux_Resolve",',
              what='counter names')
    t = e.sub(t, f, r'^REX_EXTERN\(__imp__sub_([0-9A-F]{8})\);\nREX_HOOK_RAW\(sub_\1\) \{\n'
              r'(  const std::array<uint32_t, 8> r = EntryRegs\(ctx\);\n)  __imp__sub_\1\(ctx, base\);\n'
              r'((?:.*\n)*?)\}\n',
              '#if REXNATIVE_IMMEDIATE_VS_FOUND\n'
              'REX_EXTERN(REXNATIVE_IMP(REXNATIVE_IMMEDIATE_VS));\n'
              'REX_HOOK_RAW(REXNATIVE_SUB(REXNATIVE_IMMEDIATE_VS)) {\n'
              r'\2  REXNATIVE_IMP(REXNATIVE_IMMEDIATE_VS)(ctx, base);' '\n'
              r'\3}' '\n#endif\n', what='immediate VS hook')
    return t


def names_of(p):
    names = {}
    for a, n, c in p['surface']:
        names[a] = n
    for n, a in p['functions'].items():
        if a:
            names[a] = n
    return names


def rename_comments(text, names):
    """sub_/fn_ + a known address -> its name, in comments and string literals only."""
    def ren(s):
        return re.sub(r'\b(?:sub_|fn_)([0-9A-F]{8})\b',
                      lambda m: names.get(int(m.group(1), 16), m.group(0)), s)
    out = []
    for line in text.split('\n'):
        code, sep, comment = line.partition('//')
        if sep:
            comment = ren(comment)
        code = re.sub(r'"[^"\n]*"', lambda m: ren(m.group(0)), code)
        out.append(code + sep + comment)
    return '\n'.join(out)


def leftovers(text, names):
    """Guest addresses still in code (comments stripped)."""
    found = set()
    for line in text.split('\n'):
        code = line.split('//')[0]
        for m in re.finditer(r'(?<![0-9A-Fa-f])(8[2-9A][0-9A-F]{6})(?![0-9A-Fa-f])', code):
            found.add(m.group(1))
    return sorted(found)


def build(port, name, p, out, ports=None, log=print):
    paths = prof.port_paths(port)
    src_native = paths['native']
    e = Editor()
    files = {}
    for fn in sorted(os.listdir(src_native)):
        full = os.path.join(src_native, fn)
        if os.path.isfile(full):
            with open(full, encoding='utf-8', errors='surrogateescape', newline='') as fh:
                files[fn] = fh.read()
    crlf = {k for k, v in files.items() if '\r\n' in v}
    for k in crlf:
        files[k] = files[k].replace('\r\n', '\n')
    files['hook_args.h'] = _hook_args(files['hook_args.h'], e, 'hook_args.h')
    files['d3d_state.h'] = _d3d_state_h(files['d3d_state.h'], e, 'd3d_state.h', p)
    files['d3d_state.cpp'] = _d3d_state_cpp(files['d3d_state.cpp'], e, 'd3d_state.cpp')
    files['render_targets.h'] = _render_targets_h(files['render_targets.h'], e, 'render_targets.h', p)
    files['occlusion.cpp'] = _occlusion_cpp(files['occlusion.cpp'], e, 'occlusion.cpp')
    files['resolve.cpp'] = _resolve_cpp(files['resolve.cpp'], e, 'resolve.cpp')
    files['guest_d3d.cpp'] = _guest_d3d_cpp(files['guest_d3d.cpp'], e, 'guest_d3d.cpp',
                                            p['functions']['ShaderLoad'])
    _scene(files, e)
    with open(paths['trace'], encoding='utf-8', newline='') as fh:
        trace = fh.read().replace('\r\n', '\n')
    trace = _trace(trace, e, 'rexnative_d3d_hooks.cpp')

    names = names_of(p)
    for k in list(files):
        if k.endswith(('.h', '.cpp', '.inc')):
            files[k] = rename_comments(files[k], names)
    trace = rename_comments(trace, names)

    dst = os.path.join(out, 'src', 'native')
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for k, v in files.items():
        if k in crlf:
            v = v.replace('\n', '\r\n')
        with open(os.path.join(dst, k), 'w', encoding='utf-8', errors='surrogateescape',
                  newline='') as fh:
            fh.write(v)
    with open(os.path.join(out, 'src', 'rexnative_d3d_hooks.cpp'), 'w', encoding='utf-8',
              newline='\n') as fh:
        fh.write(trace)

    left = {}
    for k, v in list(files.items()) + [('rexnative_d3d_hooks.cpp', trace)]:
        if k.endswith(('.h', '.cpp')):
            lo = leftovers(v, names)
            if lo:
                left[k] = lo
    lines = ['# renderer/: o src/native neutro', '',
             'Gerado por `python -m rexnative neutral --port %s` a partir do `src/native` e do '
             '`%s` desse port.' % (name, os.path.basename(paths['trace'])), '',
             '## Edições aplicadas (%d)' % len(e.done), '']
    lines += ['- ' + d for d in e.done]
    lines += ['', '## Edições que falharam (%d)' % len(e.problems), '']
    lines += ['- ' + d for d in e.problems] or ['- nenhuma']
    lines += ['', '## Endereços de guest que ficaram no código', '']
    lines += ['- %s: %s' % (k, ', '.join(v)) for k, v in left.items()] or ['- nenhum']
    lines.append('')
    with open(os.path.join(out, 'CHANGES.md'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\n'.join(lines))
    log('neutral sources: %d edits, %d failed, addresses left in %d files -> %s'
        % (len(e.done), len(e.problems), len(left), out))
    for pr in e.problems:
        log('  FAILED ' + pr)
    for k, v in left.items():
        log('  left in %s: %s' % (k, ', '.join(v)))
    return e, left
