#!/usr/bin/env python3
"""Regenerates src/editor/lua_api.inc, the Lua API table the script editor
completes and documents from.

Names come from LuaAPI::RegisterAll in psxsplash's src/luaapi.cpp (what the
engine really registers). Signatures and docs come from the comments above
each function in src/luaapi.hh and from psxsplash-docs' lua/api-reference.md
and lua/events.md when those are found.

    python3 tools/gen_lua_api.py --psxsplash ../psxsplash [--docs ../psxsplash-docs]
"""

import argparse
import os
import re
import subprocess
import sys

# psxlua's linit.c opens base, coroutine, table, string, bit32 and debug.
# No math, io or os on the PS1: PSXMath stands in for math.
STDLIB = [
    ("", "assert", "assert(v [, message])", "Raises an error if v is false or nil; otherwise returns all its arguments."),
    ("", "error", "error(message [, level])", "Raises an error with message."),
    ("", "getmetatable", "getmetatable(object)", "Returns the metatable of object, or nil."),
    ("", "ipairs", "ipairs(t)", "Iterates t[1], t[2], ... up to the first nil."),
    ("", "next", "next(table [, index])", "Returns the next key and value of table after index."),
    ("", "pairs", "pairs(t)", "Iterates every key and value of t."),
    ("", "pcall", "pcall(f, ...)", "Calls f in protected mode: returns true and its results, or false and the error."),
    ("", "print", "print(...)", "Prints its arguments to the console (TTY)."),
    ("", "rawequal", "rawequal(a, b)", "Compares a and b without calling __eq."),
    ("", "rawget", "rawget(t, k)", "Gets t[k] without calling __index."),
    ("", "rawlen", "rawlen(v)", "Length of a table or string without calling __len."),
    ("", "rawset", "rawset(t, k, v)", "Sets t[k] = v without calling __newindex."),
    ("", "select", "select(n, ...)", "Returns the arguments after the n-th, or their count when n is '#'."),
    ("", "setmetatable", "setmetatable(t, mt)", "Sets the metatable of t and returns t."),
    ("", "tonumber", "tonumber(e [, base])", "Converts e to a number, or nil."),
    ("", "tostring", "tostring(v)", "Converts v to a string."),
    ("", "type", "type(v)", "The type of v as a string: \"nil\", \"number\", \"string\", \"table\", ..."),
    ("", "xpcall", "xpcall(f, handler, ...)", "Calls f in protected mode with handler as the message handler."),
    ("", "collectgarbage", "collectgarbage([opt [, arg]])", "Controls the garbage collector: \"collect\", \"count\", \"step\", ..."),
    ("", "unpack", "unpack(t [, i [, j]])", "Returns t[i], ..., t[j]. Lua 5.2 keeps it as table.unpack too."),
    ("string", "byte", "string.byte(s [, i [, j]])", "Byte values of s[i..j]."),
    ("string", "char", "string.char(...)", "A string from byte values."),
    ("string", "find", "string.find(s, pattern [, init [, plain]])", "Start and end of the first match of pattern in s, or nil."),
    ("string", "format", "string.format(fmt, ...)", "Formats its arguments like C's printf. Numbers are 32-bit integers on the PS1."),
    ("string", "gmatch", "string.gmatch(s, pattern)", "Iterator over the matches of pattern in s."),
    ("string", "gsub", "string.gsub(s, pattern, repl [, n])", "Copy of s with matches of pattern replaced, and the count."),
    ("string", "len", "string.len(s)", "Length of s."),
    ("string", "lower", "string.lower(s)", "s in lower case."),
    ("string", "match", "string.match(s, pattern [, init])", "Captures of the first match of pattern in s."),
    ("string", "rep", "string.rep(s, n [, sep])", "s repeated n times."),
    ("string", "reverse", "string.reverse(s)", "s reversed."),
    ("string", "sub", "string.sub(s, i [, j])", "Substring of s from i to j."),
    ("string", "upper", "string.upper(s)", "s in upper case."),
    ("table", "concat", "table.concat(t [, sep [, i [, j]]])", "Joins the strings t[i..j] with sep."),
    ("table", "insert", "table.insert(t, [pos,] value)", "Inserts value at pos (default: the end)."),
    ("table", "pack", "table.pack(...)", "A table of the arguments with n set to their count."),
    ("table", "remove", "table.remove(t [, pos])", "Removes and returns t[pos] (default: the last element)."),
    ("table", "sort", "table.sort(t [, comp])", "Sorts t in place."),
    ("table", "unpack", "table.unpack(t [, i [, j]])", "Returns t[i], ..., t[j]."),
    ("coroutine", "create", "coroutine.create(f)", "A new coroutine running f."),
    ("coroutine", "resume", "coroutine.resume(co, ...)", "Starts or continues co."),
    ("coroutine", "running", "coroutine.running()", "The running coroutine and whether it is the main one."),
    ("coroutine", "status", "coroutine.status(co)", "\"running\", \"suspended\", \"normal\" or \"dead\"."),
    ("coroutine", "wrap", "coroutine.wrap(f)", "A function that resumes a new coroutine running f."),
    ("coroutine", "yield", "coroutine.yield(...)", "Suspends the running coroutine."),
    ("bit32", "band", "bit32.band(...)", "Bitwise and."),
    ("bit32", "bor", "bit32.bor(...)", "Bitwise or."),
    ("bit32", "bxor", "bit32.bxor(...)", "Bitwise exclusive or."),
    ("bit32", "bnot", "bit32.bnot(x)", "Bitwise not."),
    ("bit32", "btest", "bit32.btest(...)", "True if the bitwise and of the arguments is not zero."),
    ("bit32", "lshift", "bit32.lshift(x, n)", "x shifted left by n bits."),
    ("bit32", "rshift", "bit32.rshift(x, n)", "x shifted right by n bits (logical)."),
    ("bit32", "arshift", "bit32.arshift(x, n)", "x shifted right by n bits (arithmetic)."),
    ("bit32", "extract", "bit32.extract(x, field [, width])", "Bits field..field+width-1 of x."),
    ("bit32", "replace", "bit32.replace(x, v, field [, width])", "x with bits field..field+width-1 replaced by v."),
    ("bit32", "lrotate", "bit32.lrotate(x, n)", "x rotated left by n bits."),
    ("bit32", "rrotate", "bit32.rrotate(x, n)", "x rotated right by n bits."),
]
STDLIB_TABLES = ["string", "table", "coroutine", "bit32", "debug"]


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def function_body(src, header_re):
    m = re.search(header_re, src)
    if not m:
        sys.exit("gen_lua_api: %s not found" % header_re)
    i = src.index("{", m.end() - 1)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j]
    sys.exit("gen_lua_api: unbalanced braces after %s" % header_re)


def registrations(cpp):
    """[(table, name, kind, cfunc)] in registration order."""
    body = function_body(cpp, r"void LuaAPI::RegisterAll\(")
    out = []
    stack = []  # one list of (name, kind, cfunc) per open table
    pending = None  # what the last push left on the stack: ("func", cfunc) or ("value", None)
    for line in body.splitlines():
        line = line.split("//")[0].strip()
        if not line:
            continue
        m = re.match(r"L\.push\((\w+)\);", line)
        if m:
            pending = ("function", m.group(1))
            continue
        if re.match(r"L\.push(Number|Boolean|String)?\(", line):
            pending = ("constant", None)
            continue
        if line.startswith("L.newTable()"):
            stack.append([])
            continue
        m = re.match(r"(\w+)\(L\);", line)
        if m and stack:
            # A helper that fills the open table, e.g. RegisterInputConstants.
            helper = function_body(cpp, r"void LuaAPI::%s\(" % m.group(1))
            for name in re.findall(r'L\.setField\(-2,\s*"(\w+)"\)', helper):
                stack[-1].append((name, "constant", None))
            continue
        m = re.match(r'L\.setField\(-2,\s*"(\w+)"\);', line)
        if m and stack:
            kind, cfunc = pending if pending else ("constant", None)
            stack[-1].append((m.group(1), kind, cfunc))
            pending = None
            continue
        m = re.match(r'L\.setGlobal\("(\w+)"\);', line)
        if m:
            fields = stack.pop() if stack else []
            out.append((m.group(1), "", "table", None))
            for name, kind, cfunc in fields:
                out.append((m.group(1), name, kind, cfunc))
    return out


def header_comments(hh):
    """cfunc -> list of comment lines directly above its declaration."""
    out = {}
    lines = hh.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"\s*static int (\w+)\(lua_State", line)
        if not m:
            continue
        comments = []
        j = i - 1
        while j >= 0 and lines[j].strip().startswith("//"):
            text = lines[j].strip()[2:].strip()
            if text and not text.startswith("==="):
                comments.insert(0, text)
            j -= 1
        out[m.group(1)] = comments
    return out


def clean_md(s):
    s = re.sub(r"\*\*(.+?)\*\*", r"\1", s)
    s = re.sub(r"`([^`]*)`", r"\1", s)
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)
    return " ".join(s.split())


def docs_entries(md, pattern):
    """{(table, name): (signature, doc)} from ```lua blocks of bare signatures
    followed by a paragraph of prose."""
    out = {}
    lines = md.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].strip() != "```lua":
            i += 1
            continue
        j = i + 1
        block = []
        while j < len(lines) and lines[j].strip() != "```":
            block.append(lines[j].strip())
            j += 1
        sigs = [m for m in (re.match(pattern, b) for b in block if b) if m]
        i = j + 1
        if not sigs or len(sigs) != len([b for b in block if b]):
            continue
        prose = []
        k = i
        while k < len(lines) and not lines[k].strip():
            k += 1
        while k < len(lines) and lines[k].strip() and not lines[k].startswith(("```", "#", "!!!", "|", "---")):
            prose.append(lines[k].strip())
            k += 1
        doc = clean_md(" ".join(prose))
        for m in sigs:
            key = (m.group("table") or "", m.group("name"))
            sig = m.group(0).split("--")[0].strip()
            if sig.startswith("function "):
                sig = sig[len("function "):]
            out.setdefault(key, (sig, doc))
    return out


def event_entries(md):
    """{("", name): (signature, doc)} from events.md's stub definitions:
    function onX(args) followed by -- comment lines up to end."""
    out = {}
    lines = md.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"function (on\w+)\((.*)\)\s*$", line.strip())
        if not m:
            continue
        doc = []
        for body in lines[i + 1:]:
            b = body.strip()
            if not b.startswith("--"):
                break
            doc.append(b[2:].strip())
        out.setdefault(("", m.group(1)), ("%s(%s)" % (m.group(1), m.group(2)), clean_md(" ".join(doc))))
    return out


def c_str(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"')
    return '"%s"' % s


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--psxsplash", default=os.path.join(here, "..", "..", "psxsplash"))
    ap.add_argument("--docs", default=os.path.join(here, "..", "..", "psxsplash-docs"))
    ap.add_argument("-o", "--out", default=os.path.join(here, "..", "src", "editor", "lua_api.inc"))
    a = ap.parse_args()

    cpp = read(os.path.join(a.psxsplash, "src", "luaapi.cpp"))
    hh = read(os.path.join(a.psxsplash, "src", "luaapi.hh"))
    regs = registrations(cpp)
    comments = header_comments(hh)

    docs = {}
    events = {}
    ref = os.path.join(a.docs, "docs", "lua", "api-reference.md")
    if os.path.exists(ref):
        docs = docs_entries(read(ref), r"(?P<table>[A-Z]\w*)\.(?P<name>\w+)\(.*\)(\s*--.*)?$")
    ev = os.path.join(a.docs, "docs", "lua", "events.md")
    if os.path.exists(ev):
        events = event_entries(read(ev))

    rows = []
    tables = set()
    for table, name, kind, cfunc in regs:
        if kind == "table":
            tables.add(table)
            rows.append((table, "", "Table", table, "psxsplash API table."))
            continue
        sig, doc = "", ""
        lines = comments.get(cfunc, []) if cfunc else []
        if lines and lines[0].startswith(table + "." + name):
            sig, lines = lines[0], lines[1:]
        hdoc = " ".join(lines)
        dsig, ddoc = docs.get((table, name), ("", ""))
        if not sig:
            # Unknown arguments: "(...)", not "()" which would read as none.
            sig = dsig or (table + "." + name + ("(...)" if kind == "function" else ""))
        doc = ddoc or hdoc
        rows.append((table, name, "Function" if kind == "function" else "Constant", sig, doc))
    # Documented tables the engine gets from elsewhere (FixedPoint is psyqo-lua's).
    for (table, name), (sig, doc) in sorted(docs.items()):
        if table in tables:
            continue
        if not any(r[0] == table and r[1] == "" for r in rows):
            rows.append((table, "", "Table", table, "psxsplash API table."))
        rows.append((table, name, "Function", sig, doc))
    for (_, name), (sig, doc) in sorted(events.items()):
        rows.append(("", name, "Callback", "function " + sig, doc))
    for table in STDLIB_TABLES:
        rows.append((table, "", "Table", table, "Lua standard library."))
    for table, name, sig, doc in STDLIB:
        rows.append((table, name, "Function", sig, doc))

    try:
        rev = subprocess.run(["git", "-C", a.psxsplash, "log", "-1", "--format=%h %cs"],
                             capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        rev = "unknown"
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by tools/gen_lua_api.py from psxsplash %s. Do not edit.\n" % rev)
        f.write("// {table, name, kind, signature, doc}; name is empty for the table itself.\n")
        for table, name, kind, sig, doc in rows:
            f.write("{%s, %s, ApiKind::%s, %s, %s},\n" % (c_str(table), c_str(name), kind, c_str(sig), c_str(doc)))
    print("gen_lua_api: %d symbols -> %s" % (len(rows), os.path.relpath(a.out)))


if __name__ == "__main__":
    main()
