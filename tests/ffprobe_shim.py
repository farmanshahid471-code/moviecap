#!/usr/bin/env python3
# Minimal ffprobe stand-in for tests (duration + WxH), built on ffmpeg -i.
import sys, subprocess, re
a = sys.argv[1:]
if '-version' in a: print("ffprobe shim"); sys.exit(0)
path = a[-1]
err = subprocess.run(['ffmpeg', '-hide_banner', '-i', path], capture_output=True, text=True).stderr
j = ' '.join(a)
if 'format=duration' in j:
    m = re.search(r'Duration: (\d+):(\d+):(\d+\.\d+)', err)
    if not m: sys.exit(1)
    print(int(m[1])*3600 + int(m[2])*60 + float(m[3])); sys.exit(0)
if 'stream=width,height' in j:
    m = re.search(r'Video:.*?, (\d{2,5})x(\d{2,5})', err)
    if not m: sys.exit(1)
    print(f"{m[1]}x{m[2]}"); sys.exit(0)
sys.exit(1)
