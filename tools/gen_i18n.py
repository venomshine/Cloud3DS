#!/usr/bin/env python3
"""Genera source/i18n_table.h a partir de tools/i18n_tr.py.
Avisa de las cadenas _("...") sin traducir y de las traducciones sin uso."""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from i18n_tr import TR

KEY_RE = re.compile(r'(?:\b_|\bN_|\btr|ui_pill_button)\("((?:[^"\\]|\\.)*)"[,)]')

def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'

DEF_RE = re.compile(r'#define\s+(\w+)\s+"((?:[^"\\]|\\.)*)"')
MAC_RE = re.compile(r'\b_\((\w+)\)')
used = []
for f in sorted(glob.glob(os.path.join(ROOT, "source", "*.c"))):
    src = open(f, encoding="utf-8").read()
    defs = dict(DEF_RE.findall(src))
    for name in MAC_RE.findall(src):
        if name in defs and defs[name] not in used:
            used.append(defs[name])
    for m in KEY_RE.finditer(src):
        k = m.group(1).encode().decode("unicode_escape").encode("latin-1").decode("utf-8")
        if k not in used:
            used.append(k)

ok = True
for k in used:
    if k not in TR:
        print("SIN TRADUCIR:", repr(k)); ok = False
    elif k.count("%") != TR[k][0].count("%") or k.count("%") != TR[k][1].count("%"):
        print("FORMATO DISTINTO:", repr(k)); ok = False
for k in TR:
    if k not in used:
        print("SIN USO:", repr(k))

with open(os.path.join(ROOT, "source", "i18n_table.h"), "w", encoding="utf-8") as out:
    out.write("/* Generado por tools/gen_i18n.py: no editar a mano */\n")
    out.write("static const TrEntry TR_TAB[] = {\n")
    for k in used:
        en, ja = TR.get(k, ("", ""))
        out.write("\t{%s, %s, %s},\n" % (c_str(k), c_str(en), c_str(ja)))
    out.write("};\n")
print("%d cadenas" % len(used))
sys.exit(0 if ok else 1)
