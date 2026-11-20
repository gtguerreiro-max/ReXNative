# Takes one RenderDoc capture of a running game through RenderDoc's target control, so the
# game's window need not be in front (F12 needs it, and a Windows prompt can hold the focus).
# Run inside RenderDoc's Python; scripts\xenia.ps1 capture does it:
#   qrenderdoc.exe --python scripts/rdoc_trigger.py
# The target must run under RenderDoc (renderdoccmd capture): Xenia Canary, the reference. A
# ReXNative game needs none of this: its renderer takes the captures itself (scripts\live.ps1
# start -RenderDoc, capture).
# Writes to the file named by REXNATIVE_RDOC_TRIGGER_OUT: "capture <path>" for each capture, or
# what went wrong. REXNATIVE_RDOC_FRAMES (default 1): consecutive frames, a capture each (what
# changes from one frame to the next: a flicker).
import os
import sys
import time

import renderdoc as rd

out = os.environ.get('REXNATIVE_RDOC_TRIGGER_OUT', 'rdoc_trigger.log')
lines = []
try:
    # The first local target that accepts a connection (the game, not this qrenderdoc).
    ident = rd.EnumerateRemoteTargets('', 0)
    tc = None
    while ident:
        tc = rd.CreateTargetControl('', ident, 'rexnative-trigger', True)
        if tc and tc.Connected():
            lines.append('connected to %s (pid %d)' % (tc.GetTarget(), tc.GetPID()))
            break
        ident = rd.EnumerateRemoteTargets('', ident)
    if not tc or not tc.Connected():
        lines.append('no target: is the game running under RenderDoc?')
    else:
        frames = max(1, int(os.environ.get('REXNATIVE_RDOC_FRAMES', '1')))
        tc.TriggerCapture(frames)
        deadline = time.time() + 30 + 10 * frames
        got = 0
        while time.time() < deadline and got < frames:
            msg = tc.ReceiveMessage(None)
            if msg.type == rd.TargetControlMessageType.NewCapture:
                lines.append('capture %s' % msg.newCapture.path)
                got += 1
            elif msg.type == rd.TargetControlMessageType.Disconnected:
                lines.append('disconnected')
                break
            time.sleep(0.1)
        if not got:
            lines.append('no capture within 30 s')
        tc.Shutdown()
except Exception as e:  # noqa: BLE001 - the log is the only way out of qrenderdoc
    lines.append('error: %r' % e)
open(out, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
sys.exit(0)
