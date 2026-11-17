# Takes one RenderDoc capture of a running game through RenderDoc's target control, so the
# game's window need not be in front (F12 needs it, and a Windows prompt can hold the focus).
# Run inside RenderDoc's Python; scripts\live.ps1 capture does it:
#   qrenderdoc.exe --python scripts/rdoc_trigger.py
# The game must run under RenderDoc (renderdoccmd capture, scripts\live.ps1 start -RenderDoc).
# Writes to the file named by REXNATIVE_RDOC_TRIGGER_OUT: "capture <path>", or what went wrong.
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
        tc.TriggerCapture(1)
        deadline = time.time() + 30
        got = False
        while time.time() < deadline and not got:
            msg = tc.ReceiveMessage(None)
            if msg.type == rd.TargetControlMessageType.NewCapture:
                lines.append('capture %s' % msg.newCapture.path)
                got = True
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
