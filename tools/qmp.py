#!/usr/bin/env python3
"""Minimal QMP driver for GregOS end-to-end testing."""
import socket, json, time, subprocess, os, sys

SOCK = "/tmp/gregos-qmp.sock"

class QMP:
    def __init__(self, path=SOCK):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX); self.s.connect(path); break
            except OSError: time.sleep(0.2)
        else: raise SystemExit("cannot connect QMP")
        self.f = self.s.makefile("rw")
        self._read()                       # greeting
        self.cmd("qmp_capabilities")
    def _read(self):
        while True:
            line = self.f.readline()
            if not line: raise EOFError
            m = json.loads(line)
            if "event" in m: continue
            return m
    def cmd(self, execute, **args):
        self.f.write(json.dumps({"execute": execute, "arguments": args} if args
                                else {"execute": execute}) + "\n")
        self.f.flush()
        return self._read()
    # --- input ---
    def key(self, *qcodes, hold=0.05):
        for q in qcodes:
            self.cmd("send-key", keys=[{"type": "qcode", "data": q}])
            time.sleep(hold)
    def _ev(self, events): self.cmd("input-send-event", events=events)
    def mouse_rel(self, dx, dy):
        """Move in <=5px steps: the guest applies 2x acceleration at >=6px."""
        while dx or dy:
            sx = max(-5, min(5, dx)); sy = max(-5, min(5, dy))
            evs = []
            if sx: evs.append({"type": "rel", "data": {"axis": "x", "value": sx}})
            if sy: evs.append({"type": "rel", "data": {"axis": "y", "value": sy}})
            if evs: self._ev(evs)
            dx -= sx; dy -= sy
            time.sleep(0.004)
    def home_mouse(self):
        """Slam the pointer to (0,0) with large negative moves."""
        for _ in range(60):
            self._ev([{"type": "rel", "data": {"axis": "x", "value": -40}},
                      {"type": "rel", "data": {"axis": "y", "value": -40}}])
            time.sleep(0.002)
        time.sleep(0.3)
    def move_to(self, x, y):
        self.home_mouse(); self.mouse_rel(x, y); time.sleep(0.2)
    def click(self, n=1):
        for _ in range(n):
            self._ev([{"type": "btn", "data": {"down": True, "button": "left"}}])
            time.sleep(0.05)
            self._ev([{"type": "btn", "data": {"down": False, "button": "left"}}])
            time.sleep(0.12)
    def shot(self, path):
        ppm = path + ".ppm"
        if os.path.exists(ppm): os.remove(ppm)
        self.cmd("screendump", filename=ppm)
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
            time.sleep(0.1)
        subprocess.run(["convert", ppm, path], check=False)
        return path
