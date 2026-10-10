#!/usr/bin/env python3
"""Send text debug commands to the player and print its log replies.

  devcmd.py z sleep2 s f     reset stats, wait 2 s, print stats, full-repaint stress
Commands: s=stats  z=reset scan-out stats  f=3 s full repaint  d=toggle DMA fill
          p=toggle cache preload  r=reload page
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import iotdash as I  # noqa: E402

link = I.Link(echo=False)
print("ready:", link.wait_ready())


def cmd(c, wait=1.0):
    link.s.write((c + "\n").encode())
    time.sleep(0.05)
    link.echo = True
    t0 = time.time()
    while time.time() - t0 < wait:
        link.poll(0.1)
    link.echo = False


time.sleep(1)
for c in sys.argv[1:]:
    if c.startswith("sleep"):
        time.sleep(float(c[5:]))
        continue
    cmd(c, 9 if c in ("f", "P") else 4 if c == "M" else 1.5)
