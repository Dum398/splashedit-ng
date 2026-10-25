#!/usr/bin/env python3
"""Regenerates src/editor/lua_api.inc, the Lua API table the script editor
completes and documents from, and src/editor/psxsplash.d.lua, the same rows
as a lua-language-server definitions file for external editors (VS Code).

Names come from LuaAPI::RegisterAll in psxsplash's src/luaapi.cpp (what the
engine really registers). Signatures and docs come from the comments above
each function in src/luaapi.hh and from psxsplash-docs' lua/api-reference.md
and lua/events.md when those are found. FixedPoint's members come from
psyqo-lua's src/lua.cpp (the nugget submodule of psxsplash by default).
The hand-kept tables (path_api.inc, strings_api.inc: the exporter's, not
psxsplash's) are read back and go into psxsplash.d.lua only, after the rest.

    python3 tools/gen_lua_api.py --psxsplash ../psxsplash [--docs ../psxsplash-docs]
        [--psyqo-lua DIR] [-o lua_api.inc] [--defs-out psxsplash.d.lua] [--hand-kept INC ...]
"""

import argparse
import os
import re
import subprocess
import sys
import textwrap

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

# Callbacks whose arguments events.md gets wrong, as psxsplash calls them:
# {name: (arguments, sentence added to the doc)}. A Trigger component's
# script gets the trigger box's index (Lua::OnTriggerEnterScript and
# OnTriggerExitScript in src/lua.cpp push triggerIndex).
EVENT_ARGS = {
    "onTriggerEnter": ("index", "index is the trigger box's number, from 0. Goes in the script set on a Trigger component."),
    "onTriggerExit": ("index", "index is the trigger box's number, from 0. Goes in the script set on a Trigger component."),
}


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


LUA_KEYWORDS = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
    "true", "until", "while",
}

# Docs prose may carry typographic punctuation; the .d.lua stays ASCII.
ASCII_FOR = {
    "—": "-", "–": "-", "‘": "'", "’": "'", "“": '"',
    "”": '"', "…": "...", " ": " ", "×": "x", "→": "->",
}


def ascii_text(s):
    s = "".join(ASCII_FOR.get(c, c) for c in s)
    bad = sorted({c for c in s if ord(c) > 127})
    if bad:
        sys.exit("gen_lua_api: no ASCII spelling for %s" % ", ".join("U+%04X" % ord(c) for c in bad))
    return s


def paren_group(s, i):
    """(inside, end) of the balanced group opening at s[i] == "("."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] in "({":
            depth += 1
        elif s[j] in ")}":
            depth -= 1
            if depth == 0:
                return s[i + 1:j], j + 1
    return s[i + 1:], len(s)


def lua_params(inside):
    """[(name, optional)] from the text between a signature's parentheses:
    [x] is optional, a|b takes a, {..} is a table, words join into one name."""
    tokens = []
    cur, cur_opt, depth, brackets = "", None, 0, 0
    for c in inside + ",":
        if c in "({":
            depth += 1
        elif c in ")}":
            depth -= 1
        if depth == 0 and c in "[]":
            brackets += 1 if c == "[" else -1
            continue
        if depth == 0 and c == ",":
            if cur.strip():
                tokens.append((cur.strip(), cur_opt))
            cur, cur_opt = "", None
            continue
        if cur_opt is None and not c.isspace():
            cur_opt = brackets > 0
        cur += c
    out, used = [], set()
    for i, (tok, opt) in enumerate(tokens):
        if tok.endswith("..."):
            out.append(("...", False))
            break
        if tok.startswith("{"):
            name = "tbl"
        else:
            name = "_".join(re.findall(r"[A-Za-z_]\w*", tok.split("|")[0])) or "arg%d" % (i + 1)
        if name in LUA_KEYWORDS:
            name += "_"
        base, n = name, 2
        while name in used:
            name, n = "%s%d" % (base, n), n + 1
        used.add(name)
        out.append((name, opt))
    return out


def parse_signature(sig, qualified):
    """(params, overloads, returns) of "T.F(a, [b]) -> x" or "T.F(a) or
    T.F(a, b)". params is None when the signature has no parentheses;
    returns is the text after ->."""
    rest = sig[len(qualified):] if sig.startswith(qualified) else ""
    groups = []
    while rest.startswith("("):
        inside, end = paren_group(rest, 0)
        groups.append(lua_params(inside))
        rest = rest[end:]
        m = re.match(r"\s+or\s+(%s)?(?=\()" % re.escape(qualified), rest)
        if not m:
            break
        rest = rest[m.end():]
    m = re.search(r"->\s*(.*)$", rest)
    returns = m.group(1).strip() if m else ""
    if not groups:
        return None, [], returns
    return groups[0], groups[1:], returns


def doc_lines(text, width=96):
    return ["--- " + l for l in textwrap.wrap(text, width, break_long_words=False, break_on_hyphens=False)]


def function_stub(qualified, sig, doc, callback=False):
    out = doc_lines(doc)
    if out:
        out.append("---")
    out.append("--- `%s`" % sig)
    params, overloads, returns = parse_signature(sig[len("function "):] if callback else sig, qualified)
    if params is None:
        params = [("...", False)]
    for name, opt in params:
        out.append("---@param %s%s any" % (name, "?" if opt else ""))
    for alt in overloads:
        out.append("---@overload fun(%s): any" % ", ".join("%s%s: any" % (n, "?" if o else "") for n, o in alt))
    if not callback:
        out.append("---@return any" + (" # " + returns if returns else ""))
    out.append("function %s(%s) end" % (qualified, ", ".join(n for n, _ in params)))
    return out


def write_defs(path, rows, rev):
    """psxsplash.d.lua: the psxsplash rows of lua_api.inc, then the hand-kept
    rows, as lua-language-server annotations. The Lua standard library is left
    to lua-ls's own copy.
    Callbacks are plain function stubs: measured with lua-ls 3.19, a script's
    own function onCreate(self) then gives no redefinition diagnostic."""
    order, tables = [], {}
    for table, name, kind, sig, doc in rows:
        if table in STDLIB_TABLES or (table == "" and kind != "Callback"):
            continue
        if table not in tables:
            order.append(table)
            tables[table] = []
        if kind != "Table":
            tables[table].append((name, kind, sig, doc))
    out = [
        "---@meta psxsplash",
        "",
        "-- Generated by tools/gen_lua_api.py from psxsplash %s. Do not edit." % rev,
        "-- lua-language-server definitions of the psxsplash Lua API (psxlua is Lua 5.2).",
        "",
    ]
    for table in order:
        members = tables[table]
        if table == "":
            out += ["-- Event callbacks: define these in a script and the engine calls them.", ""]
            for name, kind, sig, doc in members:
                out += function_stub(name, sig, doc, callback=True) + [""]
            continue
        out.append("---@class %s" % table)
        for name, kind, sig, doc in members:
            if kind == "Constant":
                out.append("---@field %s integer%s" % (name, (" # " + doc) if doc else ""))
        out += ["%s = {}" % table, ""]
        for name, kind, sig, doc in members:
            if kind == "Function":
                out += function_stub(table + "." + name, sig, doc) + [""]
    text = ascii_text("\n".join(out).rstrip("\n") + "\n")
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(text)


def fixedpoint_entries(cpp):
    """{("FixedPoint", name): (signature, doc)} from the Lua script psyqo-lua's
    src/lua.cpp embeds to build FixedPoint; metamethods are left out and a
    method (FixedPoint:raw) is written with an explicit self."""
    m = re.search(r'fixedPointScript\[\] = R"lua\((.*?)\)lua"', cpp, re.S)
    if not m:
        return {}
    lines = m.group(1).splitlines()
    locals_ = {}
    out = {}

    def comment_above(i):
        j, text = i - 1, []
        while j >= 0 and lines[j].strip().startswith("--"):
            text.insert(0, lines[j].strip()[2:].strip())
            j -= 1
        return " ".join(text)

    for i, line in enumerate(lines):
        s = line.strip()
        lm = re.match(r"local (\w+) = function\(([^)]*)\)", s)
        if lm:
            locals_[lm.group(1)] = (lm.group(2), comment_above(i))
            continue
        am = re.match(r"FixedPoint\.(\w+) = (\w+)$", s)
        if am and am.group(2) in locals_:
            args, doc = locals_[am.group(2)]
            out[("FixedPoint", am.group(1))] = ("FixedPoint.%s(%s)" % (am.group(1), args), doc)
            continue
        fm = re.match(r"function FixedPoint([.:])(\w+)\(([^)]*)\)", s)
        if fm and not fm.group(2).startswith("__"):
            args, doc = fm.group(3), comment_above(i)
            # An argument the body defaults ("if fraction == nil then") is optional.
            names = [x.strip() for x in args.split(",") if x.strip()]
            body = "\n".join(lines[i + 1:i + 4])
            k = len(names)
            while k > 0 and re.search(r"if %s == nil then" % re.escape(names[k - 1]), body):
                k -= 1
            args = ", ".join(names[:k]) + "".join(" [, %s]" % n for n in names[k:])
            if fm.group(1) == ":":
                args = "self" + (", " + args if args.strip() else "")
                doc = (doc + ". A method: call it as fp:%s()." % fm.group(2)).lstrip(". ")
            out[("FixedPoint", fm.group(2))] = ("FixedPoint.%s(%s)" % (fm.group(2), args), doc)
    return out


INC_STR = r'"((?:[^"\\]|\\.)*)"'
INC_ROW = re.compile(r'^\{%s, %s, ApiKind::(\w+), %s, %s\},$' % (INC_STR, INC_STR, INC_STR, INC_STR), re.M)


def inc_rows(path):
    """The {table, name, kind, signature, doc} rows of a hand-kept .inc, in order."""
    text = read(path)
    rows = INC_ROW.findall(text)
    if len(rows) != sum(l.startswith("{") for l in text.splitlines()):
        sys.exit("gen_lua_api: %s has a row this script cannot read" % path)
    unescape = lambda s: re.sub(r"\\(.)", r"\1", s)
    return [(unescape(t), unescape(n), k, unescape(sig), unescape(doc)) for t, n, k, sig, doc in rows]


def c_str(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"')
    return '"%s"' % s


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--psxsplash", default=os.path.join(here, "..", "..", "psxsplash"))
    ap.add_argument("--docs", default=os.path.join(here, "..", "..", "psxsplash-docs"))
    ap.add_argument("--psyqo-lua", default=None,
                    help="psyqo-lua checkout (default: the psxsplash nugget submodule's)")
    ap.add_argument("-o", "--out", default=os.path.join(here, "..", "src", "editor", "lua_api.inc"))
    ap.add_argument("--defs-out", default=os.path.join(here, "..", "src", "editor", "psxsplash.d.lua"),
                    help="lua-language-server definitions written from the same rows")
    ap.add_argument("--hand-kept", nargs="*",
                    default=[os.path.join(here, "..", "src", "editor", f) for f in ("path_api.inc", "strings_api.inc")],
                    help="hand-kept .inc tables added to the definitions only")
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
    fixed = {}
    psyqo_lua = a.psyqo_lua or os.path.join(a.psxsplash, "third_party", "nugget", "psyqo-lua")
    fp = os.path.join(psyqo_lua, "src", "lua.cpp")
    if os.path.exists(fp):
        fixed = fixedpoint_entries(read(fp))
    else:
        print("gen_lua_api: no %s, FixedPoint gets only what the docs list" % fp, file=sys.stderr)

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
    # psyqo-lua's own signature wins, the docs' prose wins.
    for (table, name), (sig, doc) in sorted(docs.items()):
        if table in tables:
            continue
        if not any(r[0] == table and r[1] == "" for r in rows):
            rows.append((table, "", "Table", table, "psxsplash API table."))
        csig, cdoc = fixed.get((table, name), ("", ""))
        rows.append((table, name, "Function", csig or sig, doc or cdoc))
    for (table, name), (sig, doc) in fixed.items():
        if (table, name) in docs:
            continue
        if not any(r[0] == table and r[1] == "" for r in rows):
            rows.append((table, "", "Table", table, "psxsplash API table."))
        rows.append((table, name, "Function", sig, doc))
    for (_, name), (sig, doc) in sorted(events.items()):
        if name in EVENT_ARGS:
            args, more = EVENT_ARGS[name]
            sig = "%s(%s)" % (name, args)
            doc = (doc + " " + more).strip()
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
    hand = [row for inc in a.hand_kept for row in inc_rows(inc)]
    write_defs(a.defs_out, rows + hand, rev)
    print("gen_lua_api: %d symbols -> %s, %s" % (len(rows), os.path.relpath(a.out), os.path.relpath(a.defs_out)))


if __name__ == "__main__":
    main()
