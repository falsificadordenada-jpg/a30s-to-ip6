#!/usr/bin/env python3
# Reimplementacao em Python 3 (escrita por nos) do gerador da blacklist dinamica do KASAN/san:
# entrada: linhas "kext:funcao:tipo" (comentarios com #); saida: array C blacklist[].
import re
import sys

EXTRA = 5
comment = re.compile(r'#.*$')
entries = []
with open(sys.argv[1]) as f:
    for line in f:
        line = comment.sub("", line).strip()
        if not line:
            continue
        fields = line.split(":")
        if len(fields) != 3:
            continue
        kext, func, ty = fields
        kext = '"%s"' % kext if kext else "NULL"
        func = '"%s"' % func if func else "NULL"
        entries.append((kext, func, "TYPE_" + (ty or "normal").upper()))

def entry(k, fn, t):
    return "\t{\n\t\t.kext_name = %s,\n\t\t.func_name = %s,\n\t\t.type_mask = %s,\n\t}," % (k, fn, t)

print("struct blacklist_entry blacklist[] = {")
for e in entries:
    print(entry(*e))
print("")
print("\t/* Unused entries that can be populated at runtime */")
for _ in range(EXTRA):
    print(entry("NULL", "NULL", 0))
print("};")
print("")
print("static size_t blacklist_entries = %d;" % len(entries))
print("static const size_t blacklist_max_entries = %d;" % (len(entries) + EXTRA))
