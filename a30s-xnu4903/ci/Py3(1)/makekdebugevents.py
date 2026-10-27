#!/usr/bin/env python3
# Reimplementacao em Python 3 (escrita por nos) do gerador kdebugevents.h:
# le trace.codes (linhas "0xID NOME") e imprime a tabela kd_events[] em C.
import re
import sys

if len(sys.argv) < 2:
    sys.exit(1)

pat = re.compile(r'0x([0-9a-fA-F]+)\s+([^\s]*)')
table = []
with open(sys.argv[1], 'rt') as f:
    for line in f:
        m = pat.match(line)
        if m:
            table.append((int(m.group(1), 16), m.group(2)))

print("typedef struct {")
print("        uint32_t   id;")
print("        const char *name;")
print("} kd_event_t;")
print("kd_event_t kd_events[] = {")
for ev_id, name in sorted(table, key=lambda x: x[0]):
    print('        {0x%x, "%s"},' % (ev_id, name))
print("};")
