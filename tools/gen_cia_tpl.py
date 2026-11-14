#!/usr/bin/env python3
# Genera source/cia_tpl.h a partir de un CIA hecho con makerom (el propio Cloud3DS.cia):
# la cadena de certificados (pública), y un ticket y un TMD que sirven de plantilla para los CIA que
# Compartir arma a partir de una app homebrew instalada (se cambian su ID, versión, tamaños y hashes).
import struct, sys
src = sys.argv[1] if len(sys.argv) > 1 else 'Cloud3DS.cia'
d = open(src, 'rb').read()
hs, typ, ver, cs, ts, ms, metas, cons = struct.unpack('<IHHIIIIQ', d[:0x20])
al = lambda x: (x + 63) & ~63
oc = al(hs); ot = al(oc + cs); om = al(ot + ts)
assert cs == 0xA00 and ts == 0x350 and ms == 0xB34, 'se espera un CIA de makerom con un contenido'
def arr(name, b):
    s = 'static const u8 %s[0x%X] = {\n' % (name, len(b))
    for i in range(0, len(b), 16):
        s += '\t' + ', '.join('0x%02X' % x for x in b[i:i + 16]) + ',\n'
    return s + '};\n'
out = '/* Generado por tools/gen_cia_tpl.py a partir de %s: no editar a mano. */\n#pragma once\n\n' % src.split('/')[-1]
out += arr('CIA_CERTS', d[oc:oc + cs]) + '\n' + arr('CIA_TIK', d[ot:ot + ts]) + '\n' + arr('CIA_TMD', d[om:om + ms])
open('source/cia_tpl.h', 'w').write(out)
print('source/cia_tpl.h: %d + %d + %d bytes' % (cs, ts, ms))
