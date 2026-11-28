"""The progress of a long copy as the log shows it: `    N% (X MB/s)`, the line the DLC download
writes and the Manager reads (gestor/progress.py, PCT_LINE) for the phase the line before it names
(`7-Zip: <archive>`, `disc image <image>`, `STFS <package>: ...`).

A line at every 5% and, while it moves more slowly than that, at most a minute apart: Forza
Motorsport 4 (a 10.5 GB RAR, 11 GB out of its discs) took 18 minutes to extract with no line at
all, and looked stuck."""
import time

STEP = 5     # percent between two lines
QUIET = 60   # seconds after which any change is written


class Meter:
    def __init__(self, total, log, step=STEP, quiet=QUIET):
        self.total = max(int(total), 1)
        self.log, self.step, self.quiet = log, step, quiet
        self.t0 = self.at = time.time()
        self.shown = 0  # the percent of the last line

    def percent(self, pct, done=None):
        """At pct percent; done bytes of the total (pct's share of it when not given) for the rate."""
        pct = max(0, min(100, int(pct)))
        if pct <= self.shown:
            return
        now = time.time()
        if pct < (self.shown // self.step + 1) * self.step and now - self.at < self.quiet:
            return
        if done is None:
            done = self.total * pct / 100
        self.shown, self.at = pct, now
        self.log('    %d%% (%.1f MB/s)' % (pct, done / 1e6 / max(now - self.t0, 1e-3)))

    def bytes(self, done):
        """done bytes of the total written."""
        self.percent(done * 100 // self.total, done)
