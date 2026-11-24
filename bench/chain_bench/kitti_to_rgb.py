#!/usr/bin/env python3
"""KITTI PNG 시퀀스 → 프레임별 원시 RGB(.rgb) + meta.txt
   python3 kitti_to_rgb.py <png 폴더> <출력 폴더>"""
import sys, os, glob
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
files = sorted(glob.glob(os.path.join(src, "*.png")))
if not files: sys.exit("PNG 없음: " + src)
sizes = set()
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB"); sizes.add(im.size)
    with open(os.path.join(dst, f"{i:06d}.rgb"), "wb") as out: out.write(im.tobytes())
w, h = next(iter(sizes))
with open(os.path.join(dst, "meta.txt"), "w") as m:
    m.write(f"source={os.path.abspath(src)}\nframes={len(files)}\nwidth={w}\nheight={h}\nbytes_per_frame={w*h*3}\n")
print(f"{len(files)} frames, sizes={sizes}, bytes/frame={w*h*3}")
if len(sizes) != 1: sys.exit("경고: 프레임 크기가 서로 다름 — 이 시퀀스는 측정에 쓰지 말 것")
