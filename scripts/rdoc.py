# RenderDoc capture queries for ReXNative, run inside RenderDoc's Python:
#   qrenderdoc.exe --python scripts/rdoc.py      (scripts/rdoc.ps1 does it)
# The job is a JSON file named by REXNATIVE_RDOC_JOB:
#   {"capture": "...rdc", "out": "<dir>", "cmd": "actions"}
#       every action (draws, clears, copies, markers) with its event id and outputs
#   {"capture": ..., "out": ..., "cmd": "history", "x": 400, "y": 120, "event": 0,
#    "texture": 0}
#       pixel history of one texel up to `event` (0: the last action): every draw that touched
#       it, with what failed (depth, stencil, culling, discard...) and the values before and
#       after; `texture` 0 = the first colour output of the last action that has one
#   {"capture": ..., "out": ..., "cmd": "save", "event": 1234, "texture": 0}
#       the texture (0: the event's first colour output, else a resource id) as PNG, as it is
#       after that event
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
            x, y = int(job["x"]), int(job["y"])
            history = controller.PixelHistory(tex, x, y, rd.Subresource(0, 0, 0),
                                              rd.CompType.Typeless)
            flags = ["sampleMasked", "backfaceCulled", "depthClipped", "depthBoundsFailed",
                     "viewClipped", "scissorClipped", "shaderDiscarded", "depthTestFailed",
                     "stencilTestFailed", "predicationSkipped", "unboundPS"]
            names = {a.eventId: a.GetName(sfile) for a in actions}
            with open(os.path.join(out, "history-%d-%d.txt" % (x, y)), "w") as f:
                f.write("texture %s, texel %d,%d, up to event %d\n" % (describe(tex), x, y, event))
                for m in history:
                    failed = [n for n in flags if getattr(m, n, False)]
                    pre = tuple(m.preMod.col.floatValue[:4])
                    post = tuple(m.postMod.col.floatValue[:4])
                    f.write("%d %s prim %d pass=%s %s pre=(%.3g %.3g %.3g %.3g) "
                            "post=(%.3g %.3g %.3g %.3g) depth %.6g->%.6g stencil %d->%d\n" % (
                                m.eventId, names.get(m.eventId, "?"), m.primitiveID, m.Passed(),
                                ",".join(failed), *pre, *post, m.preMod.depth, m.postMod.depth,
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
            path = os.path.join(out, "tex-%d-e%d.png" % (int(tex), event))
            controller.SaveTexture(s, path)
            with open(os.path.join(out, "save.txt"), "w") as f:
                f.write("%s %s\n" % (path, describe(tex)))
        elif cmd == "inputs":
            # What the draw at `event` reads: its pixel shader's textures (the ones it used,
            # through the bindless heaps too), each saved as PNG, and its targets.
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
                saved = 0
                for u in state.GetReadOnlyResources(rd.ShaderStage.Pixel, True):
                    rid = u.descriptor.resource
                    if rid == null:
                        continue
                    f.write("ps reads %s as %s\n" % (describe(rid), u.descriptor.format.Name()))
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
