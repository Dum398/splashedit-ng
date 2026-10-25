#!/usr/bin/env python3
"""Checks a packaged editor folder the way a user meets it.

Every OS: the packaged editor's --version, read through a pipe, names an
assets folder inside the package (so the editor finds what it ships with).
Windows: splashedit.exe is a windowed program (no console window) and carries
an icon and version details, and its Start tile manifest sits beside it with
the logos it names. macOS: SplashEdit.app is a bundle Finder can open,
with its icon. Linux: the desktop entry is valid and install-desktop-entry.sh
puts it, with an absolute Exec path and the icons, in the user's menu.

    python3 tests/packagecheck.py <package folder>
"""

import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree

failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def same(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def version(exe, assets):
    r = subprocess.run([exe, "--version"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    out = r.stdout.decode("utf-8", "replace").splitlines()
    print("  " + " | ".join(out))
    check(r.returncode == 0 and len(out) >= 2 and out[0].startswith("SplashEdit "), "--version prints the version")
    found = out[1][len("assets "):] if len(out) >= 2 and out[1].startswith("assets ") else ""
    check(bool(found) and same(found, assets), f"the editor uses the packaged assets ({found})")


def peResources(exe):
    """(subsystem, resource type ids) of a PE file."""
    b = open(exe, "rb").read()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    sections, optSize = struct.unpack_from("<HH", b, pe + 6)[0], struct.unpack_from("<H", b, pe + 20)[0]
    opt = pe + 24
    subsystem = struct.unpack_from("<H", b, opt + 68)[0]
    magic = struct.unpack_from("<H", b, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    rva = struct.unpack_from("<I", b, dirs + 2 * 8)[0]
    types = set()
    for k in range(sections):
        s = opt + optSize + 40 * k
        vsize, vaddr, _, raw = struct.unpack_from("<IIII", b, s + 8)
        if rva and vaddr <= rva < vaddr + max(vsize, 1):
            root = raw + rva - vaddr
            named, ids = struct.unpack_from("<HH", b, root + 12)
            for e in range(named, named + ids):
                types.add(struct.unpack_from("<I", b, root + 16 + 8 * e)[0])
    return subsystem, types, b


def windows(pkg):
    exe = os.path.join(pkg, "splashedit.exe")
    subsystem, types, data = peResources(exe)
    check(subsystem == 2, f"splashedit.exe is a windowed program (subsystem {subsystem}, 2 = windows, 3 = console)")
    check(14 in types, "splashedit.exe carries an icon")
    check(16 in types and "SplashEdit".encode("utf-16-le") in data, "splashedit.exe carries version details")
    version(exe, os.path.join(pkg, "assets"))
    visualElements(pkg, exe)


def visualElements(pkg, exe):
    """Windows only reads <exe name>.VisualElementsManifest.xml from beside the exe, and ignores
    the whole file when a required attribute is missing or a logo it names cannot be found."""
    name = os.path.splitext(os.path.basename(exe))[0] + ".VisualElementsManifest.xml"
    path = os.path.join(os.path.dirname(exe), name)
    check(os.path.isfile(path), f"{name} is beside {os.path.basename(exe)}")
    if not os.path.isfile(path):
        return
    root = ElementTree.parse(path).getroot()
    elements = root.find("VisualElements")
    check(root.tag == "Application" and elements is not None, f"{name} is an Application with VisualElements")
    if elements is None:
        return
    a = elements.attrib
    check(re.fullmatch(r"#[0-9a-fA-F]{6}", a.get("BackgroundColor", "")) is not None
          and a.get("ShowNameOnSquare150x150Logo") in ("on", "off") and a.get("ForegroundText") in ("light", "dark"),
          f"{name} sets the tile colour, name and text ({a})")
    for logo in ("Square150x150Logo", "Square70x70Logo"):
        file = a.get(logo, "")
        check(bool(file) and os.path.isfile(os.path.join(os.path.dirname(exe), *file.split("\\"))),
              f"the {logo} {name} names is in the package ({file})")


def macos(pkg):
    app = os.path.join(pkg, "SplashEdit.app", "Contents")
    with open(os.path.join(app, "Info.plist"), "rb") as f:
        info = plistlib.load(f)
    exe = os.path.join(app, "MacOS", info.get("CFBundleExecutable", ""))
    check(info.get("CFBundlePackageType") == "APPL" and os.access(exe, os.X_OK), "SplashEdit.app runs its executable")
    icon = os.path.join(app, "Resources", info.get("CFBundleIconFile", ""))
    check(os.path.isfile(icon) and open(icon, "rb").read(4) == b"icns", "SplashEdit.app carries its icon")
    if shutil.which("plutil"):
        check(subprocess.run(["plutil", "-lint", os.path.join(app, "Info.plist")]).returncode == 0, "Info.plist lints")
    version(exe, os.path.join(app, "Resources", "assets"))


def linux(pkg):
    entry = os.path.join(pkg, "share", "applications", "splashedit.desktop")
    validate = shutil.which("desktop-file-validate")
    check(validate is not None or not os.environ.get("CI"), "desktop-file-validate is installed")
    if validate:
        check(subprocess.run([validate, entry]).returncode == 0, "the desktop entry is valid")
    for n in (16, 24, 32, 48, 64, 128, 256, 512):
        check(os.path.isfile(os.path.join(pkg, "share", "icons", "hicolor", f"{n}x{n}", "apps", "splashedit.png")),
              f"the package carries the {n}px icon")
    version(os.path.join(pkg, "splashedit"), os.path.join(pkg, "assets"))
    # Installed from a folder whose name needs quoting in a desktop entry.
    with tempfile.TemporaryDirectory() as tmp:
        moved = os.path.join(tmp, "my games", "splash $edit")
        shutil.copytree(pkg, moved, symlinks=True)
        home = os.path.join(tmp, "home")
        env = dict(os.environ, HOME=home)
        env.pop("XDG_DATA_HOME", None)
        r = subprocess.run(["sh", os.path.join(moved, "install-desktop-entry.sh")], env=env)
        check(r.returncode == 0, "install-desktop-entry.sh runs")
        data = os.path.join(home, ".local", "share")
        installed = os.path.join(data, "applications", "splashedit.desktop")
        lines = open(installed).read().splitlines() if os.path.isfile(installed) else []
        execs = [l for l in lines if l.startswith("Exec=")]
        want = 'Exec="' + os.path.join(moved, "splashedit").replace("$", "\\\\$") + '"'
        check(execs == [want], f"the menu entry starts the editor where it is ({execs})")
        check(os.path.isfile(os.path.join(data, "icons", "hicolor", "256x256", "apps", "splashedit.png")),
              "the menu entry's icon is installed")
        if validate:
            check(subprocess.run([validate, installed]).returncode == 0, "the installed desktop entry is valid")


if len(sys.argv) != 2:
    sys.exit(__doc__)
pkg = sys.argv[1]
if sys.platform == "win32":
    windows(pkg)
elif sys.platform == "darwin":
    macos(pkg)
else:
    linux(pkg)
print(f"{len(failures)} failures")
sys.exit(1 if failures else 0)
