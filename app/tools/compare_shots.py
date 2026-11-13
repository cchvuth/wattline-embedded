#!/usr/bin/env python3
"""Puts each native shot over the matching app golden, for a human to judge (the Dash arrangement differs from the
goldens on purpose; colours, fonts, sizes, glyphs and the ring must not).
usage: compare_shots.py <shots dir> <goldens root> <out dir>    (the newest *-w1536 golden set is used)"""
import glob
import os
import sys

from PIL import Image, ImageDraw

shots, root, out = sys.argv[1:4]
sets = sorted(glob.glob(os.path.join(root, '*-w1536')), key=os.path.getmtime)
if not sets:
    sys.exit(f'no *-w1536 goldens under {root}')
gold = sets[-1]
os.makedirs(out, exist_ok=True)
BAR = 28
for f in sorted(glob.glob(os.path.join(shots, '*.png'))):
    name = os.path.basename(f)
    g = os.path.join(gold, name)
    if not os.path.exists(g):
        continue
    a, b = Image.open(f).convert('RGB'), Image.open(g).convert('RGB')
    w, h = max(a.width, b.width), a.height + b.height + 2 * BAR
    im = Image.new('RGB', (w, h), (40, 40, 40))
    d = ImageDraw.Draw(im)
    d.text((8, 8), f'native (Dash arrangement): {name}', fill=(255, 255, 255))
    im.paste(a, (0, BAR))
    d.text((8, BAR + a.height + 8), f'app golden {os.path.basename(gold)}: {name}', fill=(255, 255, 255))
    im.paste(b, (0, 2 * BAR + a.height))
    im.save(os.path.join(out, name))
    print(os.path.join(out, name))
