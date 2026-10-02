#!/usr/bin/env python3
"""Writes a realistic test subtitle (10 min, ~120 cues, named characters).
usage: make_test_srt.py OUT [utf8|utf8bom|utf16le|utf16be|cp1252] [crlf]"""
import sys
out, enc = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else 'utf8')
crlf = 'crlf' in sys.argv
lines = ["Mara, wake up. The lighthouse is lit again.", "<i>That's impossible, Theo.</i>",
         "Captain Vell's map shows a door under the rocks.", "We go tonight, before the tide.",
         "Café's closed, nobody will see us.", "{\\an8}Theo, the key doesn't fit!",
         "Then we break it.", "Vell lied to all of us."]
def ts(t): return f"{t//3600:02d}:{t%3600//60:02d}:{t%60:02d},{(t*137)%1000:03d}"
cues = []
for i in range(120):
    a = 3 + i * 5
    cues.append(f"{i+1}\n{ts(a)} --> {ts(a+3)}\n{lines[i % len(lines)]}\n")
text = "\n".join(cues)
if crlf: text = text.replace("\n", "\r\n")
data = {'utf8': text.encode('utf-8'), 'utf8bom': b'\xef\xbb\xbf' + text.encode('utf-8'),
        'utf16le': b'\xff\xfe' + text.encode('utf-16-le'), 'utf16be': b'\xfe\xff' + text.encode('utf-16-be'),
        'cp1252': text.encode('cp1252')}[enc]
open(out, 'wb').write(data)
