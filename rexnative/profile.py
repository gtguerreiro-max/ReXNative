"""The game layer of a port, read from its sources (the hand-made ports are the references).

A profile is a JSON-able dict:
  functions: {canonical name: guest address or None (not hooked / not found)}
  surface:   [[address, name, category]] (d3d_surface.inc)
  offsets:   {name: device offset} (d3d_state.h dev::, render_targets.h kDev*, occlusion.cpp,
             the register shadow table of d3d_state.cpp)
  clear_color: {'address': int, 'vector': bool} (resolve.cpp, the default when pClearColor is 0)
  resolve_abi: {'hooked': 'public'|'inner', 'args': [entry register for public r3..r10, or 0]}
  scene: the scene surface the upscaler / DLSS 5 picks (upscaler.h, fidelityfx.h, frame_height.h)
  devices: {'follow_swap_device_only': bool, 'configure_at_first_device': bool}
  xex: the static libraries (xex.read)
"""
import json
import os
import re

from . import xex as xexmod

# The guest D3D functions the renderer and its hooks name (beyond d3d_surface.inc), in the
# order the layer lists them.
FUNCTIONS = [
    'Swap', 'Resolve', 'ResolveInner', 'CreateDevice', 'RestoreSegment13392', 'RestoreSegment',
    'StartSegment', 'SwitchSegment', 'MakeSpace', 'BeginVertices', 'RestorePush',
    'DrawVerticesUP', 'BeginIndexedVertices', 'DrawIndexedVerticesUP', 'DrawVertices',
    'DrawIndexedVertices', 'DrawIndx2Inline',
    'DrawIndx2Split', 'ClearF', 'Clear', 'BeginTiling', 'EndTiling', 'QueryIssue',
    'QueryGetData', 'RunCommandBuffer', 'ShaderLoad', 'ImmediateVs', 'XInputGetState',
]
# hook_args.h HookFn enumerators -> canonical names.
HOOKFN = {
    'kBeginVertices': 'BeginVertices', 'kDrawVerticesUP': 'DrawVerticesUP',
    'kBeginIndexedVertices': 'BeginIndexedVertices',
    'kDrawIndexedVerticesUP': 'DrawIndexedVerticesUP',
    'kDrawVertices': 'DrawVertices', 'kDrawIndexedVertices': 'DrawIndexedVertices',
    'kDrawIndx2Inline': 'DrawIndx2Inline', 'kDrawIndx2Split': 'DrawIndx2Split',
    'kClearF': 'ClearF', 'kClear': 'Clear', 'kResolve': 'Resolve', 'kSwap': 'Swap',
    'kRunCommandBuffer': 'RunCommandBuffer', 'kQueryIssue': 'QueryIssue',
    'kQueryGetData': 'QueryGetData',
}
# The trace file's kFn* ids -> canonical names.
KFN = {
    'kFnSwap': 'Swap', 'kFnResolve': 'Resolve', 'kFnCreateDevice': 'CreateDevice',
    'kFnRestoreSegment13392': 'RestoreSegment13392', 'kFnRestoreSegment': 'RestoreSegment',
    'kFnStartSegment': 'StartSegment', 'kFnSwitchSegment': 'SwitchSegment',
    'kFnMakeSpace': 'MakeSpace', 'kFnBeginVertices': 'BeginVertices',
    'kFnRestorePush': 'RestorePush', 'kFnDrawVerticesUP': 'DrawVerticesUP',
    'kFnDrawVertices': 'DrawVertices', 'kFnDrawIndexed': 'DrawIndexedVertices',
    'kFnClear': 'Clear', 'kFnBeginTiling': 'BeginTiling', 'kFnEndTiling': 'EndTiling',
    'kFnQueryIssue': 'QueryIssue', 'kFnQueryGetData': 'QueryGetData',
}
# Functions the tool finds by their place in the target (adapt.indexed_up), never through a
# reference: the linker drops them from games that do not use them, and a reference that has
# them would hand one to such a game on a neighbour's address, shifting the neighbours along.
POSITION_ONLY = ('BeginIndexedVertices', 'DrawIndexedVerticesUP')

# Sentinels the hand-made layers give functions they could not find (hook_args.h).
SENTINEL_MAX = 0x100

# The public Resolve signature the renderer decodes (hook_args.h): r3..r10.
RESOLVE_PUBLIC_ARGS = ['device', 'flags', 'src_rect', 'dest_texture', 'dest_point', 'level',
                       'slice', 'clear_color']


def _read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def _code(text):
    """Text with // comments removed (line by line; enough for these files)."""
    return '\n'.join(line.split('//')[0] for line in text.splitlines())


def load_ports(path):
    with open(path, encoding='utf-8') as f:
        ports = json.load(f)
    return {k: v for k, v in ports.items() if not k.startswith('_')}


def port_paths(port):
    root = port['root']
    return {k: os.path.join(root, port[k]) for k in ('native', 'trace', 'surface', 'generated',
                                                       'xex', 'layer') if k in port}


def read_surface(path):
    out = []
    for m in re.finditer(r'^D3D_FN\(([0-9A-F]{8}), "([^"]+)", (\w+)\)', _read(path), re.M):
        out.append([int(m.group(1), 16), m.group(2), m.group(3)])
    return out


def extract_layer(port, name=None):
    """Profile of a port that runs ReXNative's renderer: its game_layer.h and d3d_surface.inc
    ("layer" in ports.json), the same facts the hand-made sources held."""
    p = port_paths(port)
    layer = p['layer']
    h = _code(_read(os.path.join(layer, 'game_layer.h')))

    def const(name, default=None):
        m = re.search(r'\b%s = ([^;]+);' % re.escape(name), h)
        return m.group(1).strip() if m else default

    def num(v):
        return int(v, 0) if v is not None else None

    funcs = {}
    for n in FUNCTIONS:
        a = num(const('kFn' + n))
        funcs[n] = a or None
    if funcs.get('ResolveInner') == funcs.get('Resolve'):
        funcs['ResolveInner'] = None
    offsets = {}
    for ns in ('dev', 'shadow', 'rt', 'occ'):
        m = re.search(r'namespace %s \{(.*?)\}' % ns, h, re.S)
        for k, v in re.findall(r'constexpr uint32_t (k\w+) = (\d+);', m.group(1) if m else ''):
            offsets['%s.%s' % (ns, k[1:] if ns == 'shadow' else k)] = int(v)
    regs = re.search(r'kResolveArgRegs\[8\] = \{([^}]*)\}', h)
    args = [int(x) for x in regs.group(1).split(',')] if regs else list(range(3, 11))
    remap = const('kResolveRemapArgs') == 'true'
    call = {}
    for name in ('DrawIndexedVertices', 'DrawVertices', 'DrawVerticesUP', 'BeginVertices'):
        m = re.search(r'kArgs%s\[8\] = \{([^}]*)\}' % name, h)
        if m:
            call[name] = [int(x) for x in m.group(1).split(',')]
    scene = {
        'kSceneSurfaceMask': num(const('kSceneSurfaceMask')),
        'kSceneSurface': num(const('kSceneSurface')),
        'width': num(const('kSceneWidth')),
        'scaled_pitch': num(const('kScenePitch')),
        'height': num(const('kGameFrameHeight')),
    }
    m = re.search(r'#define REXNATIVE_SCENE_AUTO (\d)', h)
    scene['auto'] = bool(m and m.group(1) == '1')
    defaults = {
        'enabled': const('kDefaultEnabled', 'true') == 'true',
        'render_height': num(const('kDefaultRenderHeight')),
        'occlusion': const('kDefaultOcclusion') == 'true',
        'display_aspect': const('kDefaultDisplayAspect'),
        'cmdbuf': int(const('kDefaultCmdBuf', '0')),  # layers made before it: 0 (hook_args.h)
        'cmdbuf_bin_mask': num(const('kCmdBufBinMaskOffset')),  # None: hook_args.h's
    }
    clear = None
    if num(const('kResolveDefaultClearColor')):
        clear = {'address': num(const('kResolveDefaultClearColor')),
                 'vector': const('kResolveDefaultClearIsVector') == 'true'}
    info = xexmod.read(p['xex']) if os.path.exists(p.get('xex', '')) else None
    return {
        'name': name, 'title': port.get('title', name), 'generated': p['generated'], 'xex': info,
        'functions': funcs,
        'surface': read_surface(os.path.join(layer, 'd3d_surface.inc')),
        'offsets': offsets,
        'clear_color': clear,
        'resolve_abi': {'hooked': 'inner' if remap else 'public', 'args': args},
        'call_args': call,
        'scene': scene, 'defaults': defaults,
        'devices': {'follow_swap_device_only': const('kFollowSwapDeviceOnly') == 'true',
                    'configure_at_first_device': const('kConfigureAtFirstDevice') == 'true'},
        'notes': ['from its ReXNative layer (%s)' % layer],
    }


def extract(port, name=None):
    """Profile of a port (ports.json entry): from its ReXNative layer when it has one ("layer"),
    else from its hand-made sources."""
    p = port_paths(port)
    if 'layer' in p:
        return extract_layer(port, name)
    native = p['native']
    funcs = {n: None for n in FUNCTIONS}
    notes = []

    hook_args = _code(_read(os.path.join(native, 'hook_args.h')))
    for m in re.finditer(r'\b(k\w+) = 0x([0-9A-Fa-f]{8}),', hook_args):
        if m.group(1) in HOOKFN:
            v = int(m.group(2), 16)
            funcs[HOOKFN[m.group(1)]] = v if v >= SENTINEL_MAX else None

    trace = _read(p['trace'])
    trace_code = _code(trace)
    m = re.search(r'constexpr FnId (kFnSwap = .*?);', trace_code, re.S)
    aliases = {}
    if m:
        for k, v in re.findall(r'(kFn\w+) = (kFn\w+)', m.group(1)):
            if k not in KFN:
                continue
            g = re.fullmatch(r'kFn_([0-9A-F]{8})', v)
            if g:
                funcs[KFN[k]] = int(g.group(1), 16)
            elif v == 'kFnCount':
                funcs[KFN[k]] = None
            else:
                aliases[KFN[k]] = KFN.get(v, v)
    for k, v in aliases.items():
        funcs[k] = funcs.get(v)
        notes.append('%s: same function as %s' % (k, v))
    m = re.search(r'"aux_([0-9A-F]{8})"', trace_code)
    if m:
        a = int(m.group(1), 16)
        if a != funcs['Resolve']:
            funcs['ResolveInner'] = a
    m = re.search(r'^REX_HOOK_RAW\(sub_([0-9A-F]{8})\)', trace_code, re.M)
    if m:
        funcs['ImmediateVs'] = int(m.group(1), 16)
    m = re.search(r'REX_EXTERN\(sub_([0-9A-F]{8})\)', _code(_read(os.path.join(native,
                                                                                'guest_d3d.cpp'))))
    if m:
        funcs['ShaderLoad'] = int(m.group(1), 16)

    offsets = {}
    st = _code(_read(os.path.join(native, 'd3d_state.h')))
    ns = re.search(r'namespace dev \{(.*?)\n\}', st, re.S)
    for k, v in re.findall(r'constexpr uint32_t (k\w+) = (\d+);', ns.group(1) if ns else ''):
        offsets['dev.' + k] = int(v)
    stc = _code(_read(os.path.join(native, 'd3d_state.cpp')))
    for v, reg in re.findall(r'\{(\d{4,5}), 0x(2[0-9A-F]{3}),', stc):
        offsets['shadow.%s' % reg] = int(v)
    rt = _code(_read(os.path.join(native, 'render_targets.h')))
    for k, v in re.findall(r'constexpr uint32_t (kDev\w+) = (\d+);', rt):
        offsets['rt.' + k] = int(v)
    occ = _code(_read(os.path.join(native, 'occlusion.cpp')))
    m = re.search(r'kDevFenceNext = (\d+), kDevFenceDone = (\d+);', occ)
    if m:
        offsets['occ.kDevFenceNext'] = int(m.group(1))
        offsets['occ.kDevFenceDone'] = int(m.group(2))

    res = _code(_read(os.path.join(native, 'resolve.cpp')))
    clear = None
    m = re.search(r'LoadFloat\(base, 0x([0-9A-Fa-f]{8})u( \+ 4 \* i)?\)', res)
    if m:
        clear = {'address': int(m.group(1), 16), 'vector': bool(m.group(2))}

    args = list(range(3, 11))
    hooked = 'public'
    m = re.search(r'if \(a\.fn == [\w:]*HookFn::kResolve\) \{(.*?)\n  \}', trace_code, re.S)
    if m:
        for i, src in re.findall(r'a\.r\[(\d)\] = (ctx\.r\d+\.u32|0);', m.group(1)):
            g = re.match(r'ctx\.r(\d+)', src)
            args[int(i)] = int(g.group(1)) if g else 0
        hooked = 'inner'
    elif funcs['ResolveInner'] and funcs['Resolve'] == funcs['ResolveInner']:
        hooked = 'inner'
    resolve_abi = {'hooked': hooked, 'args': args}

    scene = {}
    up = _code(_read(os.path.join(native, 'upscaler.h')))
    for k in ('kSceneSurfaceMask', 'kSceneSurface'):
        m = re.search(k + r' = 0x([0-9A-Fa-f]+);', up)
        if m:
            scene[k] = int(m.group(1), 16)
    m = re.search(r'kFfxSceneWidth = (\d+);', _read(os.path.join(native, 'fidelityfx.h')))
    if m:
        scene['width'] = int(m.group(1))
    m = re.search(r'kGameFrameHeight = (\d+);', _read(os.path.join(native, 'frame_height.h')))
    if m:
        scene['height'] = int(m.group(1))
    m = re.search(r'kScaledPitch = (\d+);', rt)
    if m:
        scene['scaled_pitch'] = int(m.group(1))
    ren = _read(os.path.join(native, 'renderer.cpp'))
    defaults = {}
    m = re.search(r'REXCVAR_DEFINE_INT32\(pgr4_render_height, (\d+)', ren)
    if m:
        defaults['render_height'] = int(m.group(1))
    m = re.search(r'REXCVAR_DEFINE_BOOL\(pgr4_native_occlusion, (true|false)', ren)
    if m:
        defaults['occlusion'] = m.group(1) == 'true'
    pres = _read(os.path.join(native, 'present.cpp'))
    m = re.search(r'pgr4_native_display_aspect, ([^,]+),', pres)
    if m:
        defaults['display_aspect'] = m.group(1).strip()

    devices = {
        'follow_swap_device_only': 'g_main_device' in trace_code,
        'configure_at_first_device': funcs['CreateDevice'] is None,
    }

    info = None
    if os.path.exists(p.get('xex', '')):
        info = xexmod.read(p['xex'])
    return {
        'name': name,
        'title': port.get('title', name),
        'generated': p['generated'],
        'xex': info,
        'functions': funcs,
        'surface': read_surface(p['surface']),
        'offsets': offsets,
        'clear_color': clear,
        'resolve_abi': resolve_abi,
        'scene': scene,
        'defaults': defaults,
        'devices': devices,
        'notes': notes,
    }


def hexify(obj):
    """JSON-friendly copy with guest addresses as 8-digit hex strings."""
    def h(v):
        return '%08X' % v if isinstance(v, int) and v >= 0x80000000 else v
    out = json.loads(json.dumps(obj))
    out['functions'] = {k: h(v) for k, v in obj['functions'].items()}
    out['surface'] = [[h(a), n, c] for a, n, c in obj['surface']]
    if obj.get('clear_color'):
        out['clear_color'] = dict(obj['clear_color'], address=h(obj['clear_color']['address']))
    return out


def unhexify(obj):
    def u(v):
        return int(v, 16) if isinstance(v, str) and re.fullmatch(r'[0-9A-F]{8}', v) else v
    out = dict(obj)
    out['functions'] = {k: u(v) for k, v in obj['functions'].items()}
    out['surface'] = [[u(a), n, c] for a, n, c in obj['surface']]
    if obj.get('clear_color'):
        out['clear_color'] = dict(obj['clear_color'], address=u(obj['clear_color']['address']))
    return out


def as_reference(p):
    """The profile as a reference for another game: without the POSITION_ONLY functions, in its
    function table or its surface."""
    funcs = dict(p['functions'])
    drop = {funcs.get(n) for n in POSITION_ONLY} - {None}
    for n in POSITION_ONLY:
        funcs[n] = None
    surface = [e for e in p['surface'] if e[0] not in drop and e[1] not in POSITION_ONLY]
    return dict(p, functions=funcs, surface=surface)


def save(profile, path):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(hexify(profile), f, indent=1)
        f.write('\n')


def load(path):
    with open(path, encoding='utf-8') as f:
        return unhexify(json.load(f))
