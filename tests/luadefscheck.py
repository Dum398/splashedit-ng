#!/usr/bin/env python3
"""Checks src/editor/psxsplash.d.lua against src/editor/lua_api.inc, both
written by tools/gen_lua_api.py from the same rows, plus the hand-kept
path_api.inc and strings_api.inc that it adds to the definitions only.

Every psxsplash table is a ---@class with its global; every constant a
---@field of its table's class; every function a function stub with a
---@return; every callback a function stub. The Lua standard library rows
are not re-declared (lua-language-server has its own), and nothing in the
file is a function the table does not have. The file is ASCII.

    python3 tests/luadefscheck.py [lua_api.inc psxsplash.d.lua [hand-kept.inc ...]]
"""

import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
inc = sys.argv[1] if len(sys.argv) > 2 else os.path.join(here, '..', 'src', 'editor', 'lua_api.inc')
defs = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '..', 'src', 'editor', 'psxsplash.d.lua')
hand = sys.argv[3:] if len(sys.argv) > 2 else [os.path.join(here, '..', 'src', 'editor', f) for f in ('path_api.inc', 'strings_api.inc')]
STDLIB_TABLES = {'string', 'table', 'coroutine', 'bit32', 'debug'}
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def inc_rows(path):
    out = []
    for line in open(path, encoding='utf-8'):
        m = re.match(r'\{"((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", ApiKind::(\w+),', line)
        if m:
            out.append((m.group(1), m.group(2), m.group(3)))
    return out


rows = inc_rows(inc)
check('lua_api.inc has rows', len(rows) > 300, len(rows))
hand_rows = [r for h in hand for r in inc_rows(h)]
check('the hand-kept tables have rows', all(inc_rows(h) for h in hand), hand)
rows += hand_rows
raw = open(defs, 'rb').read()
text = raw.decode('ascii', errors='replace')
lines = text.splitlines()

check('psxsplash.d.lua is ASCII', all(b < 128 for b in raw), [i for i, b in enumerate(raw) if b >= 128][:5])
check('psxsplash.d.lua is a lua-ls meta file', lines[:1] == ['---@meta psxsplash'], lines[:1])

# The ---@field lines of each class: from ---@class T to T = {}.
fields, current = {}, None
for l in lines:
    m = re.match(r'---@class (\w+)$', l)
    if m:
        current = m.group(1)
        fields[current] = set()
        continue
    m = re.match(r'---@field (\w+) ', l)
    if m and current:
        fields[current].add(m.group(1))
        continue
    if current and l == '%s = {}' % current:
        current = None

stubs = {}  # "T.F" or "onX" -> index of its function line
for i, l in enumerate(lines):
    m = re.match(r'function ([\w.]+)\(', l)
    if m:
        stubs[m.group(1)] = i

missing, stdlib = [], []
psx = [r for r in rows if r[0] not in STDLIB_TABLES and not (r[0] == '' and r[2] != 'Callback')]
for table, name, kind in rows:
    if table in STDLIB_TABLES or (table == '' and kind == 'Function'):
        key = table + '.' + name if table else name
        if name and key in stubs:
            stdlib.append(key)
        continue
    if kind == 'Table':
        if table not in fields or '%s = {}' % table not in lines:
            missing.append('table ' + table)
    elif kind == 'Constant':
        if name not in fields.get(table, set()):
            missing.append('constant %s.%s' % (table, name))
    elif kind == 'Function':
        i = stubs.get(table + '.' + name)
        if i is None or not lines[i - 1].startswith('---@return any'):
            missing.append('function %s.%s' % (table, name))
    elif kind == 'Callback':
        if name not in stubs:
            missing.append('callback ' + name)
check('every psxsplash row of lua_api.inc is in psxsplash.d.lua (%d rows)' % len(psx), not missing, missing[:10])
check('no Lua standard library function is re-declared', not stdlib, stdlib[:10])
check('no stdlib table is declared', not (STDLIB_TABLES & set(fields)), sorted(STDLIB_TABLES & set(fields)))

known = {(t + '.' + n if t else n) for t, n, k in psx if k in ('Function', 'Callback')}
extra = sorted(set(stubs) - known)
check('every function in psxsplash.d.lua is a row of lua_api.inc or a hand-kept table', not extra, extra[:10])

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
