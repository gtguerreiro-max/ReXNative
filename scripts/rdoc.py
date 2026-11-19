# RenderDoc capture queries for ReXNative, run inside RenderDoc's Python:
#   qrenderdoc.exe --python scripts/rdoc.py      (scripts/rdoc.ps1 does it)
# The job is a JSON file named by REXNATIVE_RDOC_JOB:
#   {"capture": "...rdc", "out": "<dir>", "cmd": "actions"}
#       every action (draws, clears, copies, markers) with its event id and outputs
#   {"capture": ..., "out": ..., "cmd": "history", "x": 400, "y": 120, "event": 0,
#    "texture": 0, "x1": 0}
#       pixel history of one texel up to `event` (0: the last action): every draw that touched
#       it, with what failed (depth, stencil, culling, discard...) and the values before and
#       after; `texture` 0 = the first colour output of the last action that has one; with
#       `x1`, of each texel from x to x1 on the row, one after another in the same file
#   {"capture": ..., "out": ..., "cmd": "save", "event": 1234, "texture": 0}
#       the texture (0: the event's first colour output, else a resource id) as PNG, as it is
#       after that event
#   "inputs", "mesh", "disasm", "cbuffers", "state" (with "event"): the draw's textures as PNG,
#       its vertex shader's output positions, its shaders disassembled, its constant buffers,
#       its depth, stencil, depth bias and viewport; "buffer" ("offset", "size" in hex): bytes
#       of a buffer; "pick" (with "event", "texture", "x", "x1", "y"): float texel values;
#       "debug" (with "event", "x", "y"): the pixel shader run in RenderDoc's debugger
# Writes <out>/<cmd>*.txt / .png and <out>/rdoc.log ("ok" or the traceback).
import json
import os
import traceback


def open_capture(rd, path):
    cap = rd.OpenCaptureFile()
    status = cap.OpenFile(path, "", None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open %s: %s" % (path, status))
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    return cap, controller


def flatten(actions):
    for a in actions:
        yield a
        yield from flatten(a.children)


def main():
    import renderdoc as rd
    job = json.load(open(os.environ["REXNATIVE_RDOC_JOB"], encoding="utf-8-sig"))
    out = job["out"]
    os.makedirs(out, exist_ok=True)
    cap, controller = open_capture(rd, job["capture"])
    try:
        textures = {t.resourceId: t for t in controller.GetTextures()}
        sfile = controller.GetStructuredFile()
        actions = list(flatten(controller.GetRootActions()))
        null = rd.ResourceId.Null()

        def describe(rid):
            t = textures.get(rid)
            return "%s %dx%d %s" % (int(rid), t.width, t.height, t.format.Name()) if t else str(int(rid))

        def last_event():
            return max(a.eventId for a in actions)

        def output_of(event):
            # The first colour output of the action at `event`, or of the last one before it
            # that has one.
            best = None
            for a in actions:
                if a.eventId <= event and a.outputs and a.outputs[0] != null:
                    best = a.outputs[0]
            return best

        cmd = job["cmd"]
        if cmd == "actions":
            with open(os.path.join(out, "actions.txt"), "w") as f:
                for a in actions:
                    outs = [describe(o) for o in list(a.outputs) + [a.depthOut] if o != null]
                    f.write("%d %s n=%d i=%d | %s\n" % (a.eventId, a.GetName(sfile), a.numIndices,
                                                          a.numInstances, "; ".join(outs)))
        elif cmd == "history":
            event = job.get("event") or last_event()
            rid = job.get("texture") or 0
            tex = next((t for t in textures if int(t) == rid), None) if rid else output_of(event)
            controller.SetFrameEvent(event, True)
            x0, y = int(job["x"]), int(job["y"])
            x1 = max(int(job.get("x1") or 0), x0)
            flags = ["sampleMasked", "backfaceCulled", "depthClipped", "depthBoundsFailed",
                     "viewClipped", "scissorClipped", "shaderDiscarded", "depthTestFailed",
                     "stencilTestFailed", "predicationSkipped", "unboundPS"]
            names = {a.eventId: a.GetName(sfile) for a in actions}
            with open(os.path.join(out, "history-%d-%d.txt" % (x0, y)), "w") as f:
                for x in range(x0, x1 + 1):
                    history = controller.PixelHistory(tex, x, y, rd.Subresource(0, 0, 0),
                                                      rd.CompType.Typeless)
                    f.write("texture %s, texel %d,%d, up to event %d\n" %
                            (describe(tex), x, y, event))
                    for m in history:
                        failed = [n for n in flags if getattr(m, n, False)]
                        pre = tuple(m.preMod.col.floatValue[:4])
                        post = tuple(m.postMod.col.floatValue[:4])
                        shader = tuple(m.shaderOut.col.floatValue[:4])  # the pixel shader's color
                        f.write("%d %s prim %d pass=%s %s pre=(%.3g %.3g %.3g %.3g) "
                                "post=(%.3g %.3g %.3g %.3g) shader=(%.3g %.3g %.3g %.3g) "
                                "depth %.6g->%.6g (fragment %.6g) stencil %d->%d\n" % (
                                    m.eventId, names.get(m.eventId, "?"), m.primitiveID,
                                    m.Passed(), ",".join(failed), *pre, *post, *shader,
                                    m.preMod.depth, m.postMod.depth, m.shaderOut.depth,
                                    m.preMod.stencil, m.postMod.stencil))
        elif cmd == "save":
            event = job.get("event") or last_event()
            rid = job.get("texture") or 0
            tex = next((t for t in textures if int(t) == rid), None) if rid else output_of(event)
            controller.SetFrameEvent(event, True)
            s = rd.TextureSave()
            s.resourceId = tex
            s.destType = rd.FileType.PNG
            s.alpha = rd.AlphaMapping.Discard
            # -X: a mip level (0 by default).
            mip = int(job.get("x") or 0)
            s.mip = mip
            path = os.path.join(out, "tex-%d-e%d%s.png" % (int(tex), event,
                                                           "-mip%d" % mip if mip else ""))
            controller.SaveTexture(s, path)
            with open(os.path.join(out, "save.txt"), "w") as f:
                f.write("%s %s\n" % (path, describe(tex)))
        elif cmd == "pick":
            # The values of texture `texture` at (x, y), (x1, y) and a few points between, after
            # the action at `event`: float data that a PNG clamps.
            controller.SetFrameEvent(job["event"], True)
            rid = next(r for r in textures if int(r) == job["texture"])
            t = textures[rid]
            xs = sorted({job["x"], job["x1"] or job["x"]} |
                        {int(t.width * f) for f in (0.1, 0.3, 0.5, 0.7, 0.9)})
            ys = sorted({job["y"]} | {int(t.height * f) for f in (0.1, 0.3, 0.5, 0.7, 0.9)})
            with open(os.path.join(out, "pick-e%d-%d.txt" % (job["event"], job["texture"])), "w") as f:
                f.write("%s after event %d\n" % (describe(rid), job["event"]))
                for y in ys:
                    for x in xs:
                        v = controller.PickPixel(rid, x, y, rd.Subresource(0, 0, 0),
                                                 rd.CompType.Float)
                        f.write("(%d,%d) %s\n" % (x, y, " ".join("%.4g" % c for c in v.floatValue)))
        elif cmd == "debug":
            # The pixel shader of the draw at `event` run at (x, y) in RenderDoc's debugger: its
            # inputs, then each instruction's changed registers, into debug-e<event>-x-y.txt.
            controller.SetFrameEvent(job["event"], True)

            def show(v):
                vals = getattr(v.value, "f32v", None) or getattr(v.value, "f", [])
                return "%s = %s" % (v.name, " ".join("%.6g" % c for c in
                                                     list(vals)[:max(v.columns, 1)]))

            trace = controller.DebugPixel(job["x"], job["y"], rd.DebugPixelInputs())
            path = os.path.join(out, "debug-e%d-%d-%d.txt" % (job["event"], job["x"], job["y"]))
            with open(path, "w") as f:
                if trace is None or trace.debugger is None:
                    f.write("no pixel shader ran there\n")
                else:
                    for v in trace.inputs:
                        f.write("in %s\n" % show(v))
                    steps = 0
                    while steps < 20000:
                        states = controller.ContinueDebug(trace.debugger)
                        if not states:
                            break
                        for s in states:
                            steps += 1
                            for c in s.changes:
                                f.write("%d: %s\n" % (s.nextInstruction, show(c.after)))
                    controller.FreeTrace(trace)
        elif cmd == "reads":
            # Every draw from `event` on (0: all): what its pixel shader reads and where it
            # draws, one line each, to follow an image through a frame's passes.
            with open(os.path.join(out, "reads-e%d.txt" % job["event"]), "w") as f:
                for a in actions:
                    if a.eventId < job["event"] or not (a.flags & rd.ActionFlags.Drawcall):
                        continue
                    controller.SetFrameEvent(a.eventId, False)
                    state = controller.GetPipelineState()
                    reads = [describe(u.descriptor.resource)
                             for u in state.GetReadOnlyResources(rd.ShaderStage.Pixel, True)
                             if u.descriptor.resource != null]
                    outs = [describe(o) for o in list(a.outputs) + [a.depthOut] if o != null]
                    f.write("%d n=%d ps %d | reads %s | -> %s\n" % (
                        a.eventId, a.numIndices, int(state.GetShader(rd.ShaderStage.Pixel)),
                        ", ".join(reads) or "-", "; ".join(outs)))
        elif cmd == "inputs":
            # What the draw at `event` reads: its shaders' textures (the ones they used, through
            # the bindless heaps too), each saved as PNG, and its targets.
            event = job["event"]
            controller.SetFrameEvent(event, True)
            state = controller.GetPipelineState()
            action = next(a for a in actions if a.eventId == event)
            with open(os.path.join(out, "inputs-e%d.txt" % event), "w") as f:
                f.write("%d %s\n" % (event, action.GetName(sfile)))
                for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
                    f.write("%s shader %s\n" % (stage, int(state.GetShader(stage))))
                f.write("outputs: %s\n" % "; ".join(
                    describe(o) for o in list(action.outputs) + [action.depthOut] if o != null))
                for i, b in enumerate(state.GetColorBlends()[:4]):
                    if not b.enabled and i:
                        continue
                    f.write("blend %d: enabled=%d color %s * %s %s alpha %s * %s %s mask %X\n" % (
                        i, b.enabled, b.colorBlend.source, b.colorBlend.destination,
                        b.colorBlend.operation, b.alphaBlend.source, b.alphaBlend.destination,
                        b.alphaBlend.operation, b.writeMask))
                saved = 0
                # The vertex stage's too (the domain shader's with the tessellator): its vertex
                # texture fetches.
                reads = [(tag, u) for stage, tag in ((rd.ShaderStage.Vertex, "vs"),
                                                     (rd.ShaderStage.Domain, "ds"),
                                                     (rd.ShaderStage.Pixel, "ps"))
                         if state.GetShaderReflection(stage) is not None
                         for u in state.GetReadOnlyResources(stage, True)]
                for tag, u in reads:
                    rid = u.descriptor.resource
                    if rid == null:
                        continue
                    f.write("%s reads %s as %s\n" % (tag, describe(rid),
                                                     u.descriptor.format.Name()))
                    t = textures.get(rid)
                    if t and saved < 12:
                        s = rd.TextureSave()
                        s.resourceId = rid
                        s.destType = rd.FileType.PNG
                        s.alpha = rd.AlphaMapping.Discard
                        path = os.path.join(out, "in-e%d-%d.png" % (event, int(rid)))
                        controller.SaveTexture(s, path)
                        f.write("  saved %s\n" % path)
                        saved += 1
        elif cmd == "mesh":
            # The vertex shader's output positions of the draw at `event` (up to 64 vertices):
            # clip x y z w and the pixel they land on in the viewport, into mesh-e<event>.txt.
            import struct
            event = job["event"]
            controller.SetFrameEvent(event, True)
            vp = controller.GetPipelineState().GetViewport(0)
            mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            stage = "vertex shader"
            # A tessellated (or geometry shader) draw: its positions are the last stage's, the
            # domain shader's; the vertex shader then only passes an index on.
            if mesh.vertexByteStride < 16:
                mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.GSOut)
                stage = "domain or geometry shader"
            with open(os.path.join(out, "mesh-e%d.txt" % event), "w") as f:
                f.write("%d %s: %d vertices (%s), stride %d, viewport %g,%g %gx%g\n" % (
                    event, next(a for a in actions if a.eventId == event).GetName(sfile),
                    mesh.numIndices, stage, mesh.vertexByteStride, vp.x, vp.y, vp.width,
                    vp.height))
                n = min(mesh.numIndices, 64)
                if mesh.vertexResourceId != null and n:
                    data = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset,
                                                    mesh.vertexByteStride * n)
                    # An indexed draw's output holds each vertex once: fewer than its indices.
                    n = min(n, len(data) // mesh.vertexByteStride)
                    for i in range(n):
                        x, y, z, w = struct.unpack_from("<4f", data, i * mesh.vertexByteStride)
                        sx = vp.x + (x / w * 0.5 + 0.5) * vp.width if w else float("nan")
                        sy = vp.y + (0.5 - y / w * 0.5) * vp.height if w else float("nan")
                        rest = struct.unpack_from("<%df" % (mesh.vertexByteStride // 4 - 4), data,
                                                  i * mesh.vertexByteStride + 16)
                        f.write("%d clip %.4g %.4g %.4g %.4g pixel %.1f %.1f | %s\n" % (
                            i, x, y, z, w, sx, sy, " ".join("%.4g" % v for v in rest)))
        elif cmd == "disasm":
            # The shaders of the draw at `event`, disassembled (DXBC), into
            # disasm-e<event>-vs.txt / -hs / -ds / -gs / -ps.txt (the stages it has).
            event = job["event"]
            controller.SetFrameEvent(event, True)
            state = controller.GetPipelineState()
            pipe = state.GetGraphicsPipelineObject()
            targets = controller.GetDisassemblyTargets(True)
            for stage, tag in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Hull, "hs"),
                               (rd.ShaderStage.Domain, "ds"), (rd.ShaderStage.Geometry, "gs"),
                               (rd.ShaderStage.Pixel, "ps")):
                refl = state.GetShaderReflection(stage)
                if refl is None:
                    continue
                text = controller.DisassembleShader(pipe, refl, targets[0])
                open(os.path.join(out, "disasm-e%d-%s.txt" % (event, tag)), "w").write(text)
        elif cmd == "state":
            # The draw's depth and stencil test, depth bias and viewport (D3D12), into
            # state-e<event>.txt. Each field read on its own: the API's names move between
            # RenderDoc versions.
            event = job["event"]
            controller.SetFrameEvent(event, True)
            d = controller.GetD3D12PipelineState()
            lines = ["event %d" % event]

            def field(label, get):
                try:
                    lines.append("%s %s" % (label, get()))
                except Exception as e:  # noqa: BLE001
                    lines.append("%s ? (%s)" % (label, e))

            om = lambda: d.outputMerger.depthStencilState  # noqa: E731
            rs = lambda: d.rasterizer.state  # noqa: E731
            field("depth enable", lambda: om().depthEnable)
            field("depth write", lambda: om().depthWrites)
            field("depth func", lambda: om().depthFunction)
            field("stencil enable", lambda: om().stencilEnable)
            field("stencil front", lambda: (om().frontFace.function, om().frontFace.passOperation,
                                            om().frontFace.depthFailOperation))
            field("depth bias", lambda: rs().depthBias)
            field("slope bias", lambda: rs().slopeScaledDepthBias)
            field("bias clamp", lambda: rs().depthBiasClamp)
            field("depth clip", lambda: rs().depthClip)
            field("cull", lambda: (rs().cullMode, rs().frontCCW))
            field("viewport0", lambda: (d.rasterizer.viewports[0].x, d.rasterizer.viewports[0].y,
                                        d.rasterizer.viewports[0].width,
                                        d.rasterizer.viewports[0].height,
                                        d.rasterizer.viewports[0].minDepth,
                                        d.rasterizer.viewports[0].maxDepth))
            field("scissor0", lambda: (d.rasterizer.scissors[0].x, d.rasterizer.scissors[0].y,
                                       d.rasterizer.scissors[0].width,
                                       d.rasterizer.scissors[0].height))
            open(os.path.join(out, "state-e%d.txt" % event), "w").write(chr(10).join(lines) + chr(10))
        elif cmd == "cbuffers":
            # The constant buffers bound to the draw at `event`'s shaders, as float4s (up to 64
            # each), into cbuffers-e<event>.txt.
            import struct
            event = job["event"]
            controller.SetFrameEvent(event, True)
            state = controller.GetPipelineState()
            with open(os.path.join(out, "cbuffers-e%d.txt" % event), "w") as f:
                for stage, tag in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Hull, "hs"),
                                   (rd.ShaderStage.Domain, "ds"), (rd.ShaderStage.Pixel, "ps")):
                    refl = state.GetShaderReflection(stage)
                    if refl is None:
                        continue
                    for i, cb in enumerate(refl.constantBlocks):
                        bind = state.GetConstantBlock(stage, i, 0).descriptor
                        size = min(bind.byteSize or 1024, 1024)
                        if bind.resource == null:
                            f.write("%s cb%d %s: unbound\n" % (tag, i, cb.name))
                            continue
                        data = controller.GetBufferData(bind.resource, bind.byteOffset, size)
                        f.write("%s cb%d %s (register %d, %d bytes)\n" % (
                            tag, i, cb.name, cb.fixedBindNumber, len(data)))
                        # Fetch constants are bit fields: as hex dwords.
                        fetch = "fetch" in cb.name
                        for k in range(len(data) // 16):
                            f.write("  [%d] %s\n" % (k, " ".join(
                                ("%08X" if fetch else "%.6g") % v
                                for v in struct.unpack_from("<4I" if fetch else "<4f", data,
                                                            k * 16))))
        elif cmd == "buffer":
            # Bytes of a buffer as they are after `event` (0: the last action): resource id
            # `texture`, or the largest buffer (the renderer's copy of guest physical memory, so
            # `offset` is a physical address there), into buffer-<offset>.bin.
            event = job.get("event") or last_event()
            controller.SetFrameEvent(event, True)
            buffers = controller.GetBuffers()
            rid = job.get("texture") or 0
            buf = (next(b for b in buffers if int(b.resourceId) == rid) if rid else
                   max(buffers, key=lambda b: b.length))
            offset, size = int(job["offset"], 16), int(job["size"], 16)
            data = controller.GetBufferData(buf.resourceId, offset, size)
            path = os.path.join(out, "buffer-%08X.bin" % offset)
            open(path, "wb").write(bytes(data))
            with open(os.path.join(out, "buffer.txt"), "w") as f:
                f.write("%s buffer %d (%d bytes) at %08X, %d bytes after event %d\n" % (
                    path, int(buf.resourceId), buf.length, offset, len(data), event))
        else:
            raise RuntimeError("unknown cmd " + cmd)
    finally:
        controller.Shutdown()
        cap.Shutdown()


# The log sits next to the job file (the job's "out"), so even a bad job is reported.
log = os.path.join(os.path.dirname(os.environ.get("REXNATIVE_RDOC_JOB", ".")), "rdoc.log")
try:
    main()
    open(log, "w").write("ok\n")
except Exception:
    open(log, "w").write(traceback.format_exc())
os._exit(0)
