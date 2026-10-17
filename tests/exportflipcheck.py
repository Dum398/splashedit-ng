# Export Disc Image from the editor, after the viewport has drawn the scene's
# textures, must write the same scene files as splashpack-cli. The viewport
# loads its textures bottom row first; that must not leak into the images
# export decodes. Compares SCENE_0.{SPK,VRM,SPU} read back from both discs
# (the ISO volume dates differ between runs, so not the whole .bin).
# Also: the status-bar notice's Show in folder link and Build > Show Disc
# Image in Folder print `reveal: <disc>`; the menu item is disabled before
# the first export.
# usage: python3 exportflipcheck.py <splashedit> <splashpack-cli>   (needs a display: xvfb-run -a)
import os, shutil, struct, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
cli = os.path.abspath(sys.argv[2])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
tmp = tempfile.mkdtemp()
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


# Stand-in CD-ROM engine, as disccheck.py makes it.
engine = os.path.join(tmp, 'cd.ps-exe')
open(engine, 'wb').write(b'PS-X EXE' + bytes(2040) + b'code' * 300 + b'SCENE%d/SCENE_%d.SPK;1\0' + bytes(100))


def iso_files(path):
    """name -> bytes for every file on a raw Mode 2 Form 1 disc image."""
    raw = open(path, 'rb').read()
    sec = [raw[i * 2352 + 24:i * 2352 + 24 + 2048] for i in range(len(raw) // 2352)]

    def read(lba, size):
        return b''.join(sec[lba:lba + (size + 2047) // 2048])[:size]

    def walk(lba, size, prefix):
        data, i, out = read(lba, size), 0, {}
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            ext, sz = struct.unpack_from('<I', data, i + 2)[0], struct.unpack_from('<I', data, i + 10)[0]
            name = data[i + 33:i + 33 + data[i + 32]]
            if name not in (b'\0', b'\1'):
                full = prefix + name.decode()
                if data[i + 25] & 2:
                    out.update(walk(ext, sz, full + '/'))
                else:
                    out[full] = read(ext, sz)
            i += n
        return out

    root = sec[16][156:156 + 34]
    return walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')


project = os.path.join(tmp, 'courtyard')
shutil.copytree(example, project)
scene = os.path.join(project, 'courtyard.scene')

cli_bin = os.path.join(tmp, 'cli.bin')
r = subprocess.run([cli, 'disc', scene, '--engine', engine, '-o', cli_bin], capture_output=True, text=True)
check('splashpack-cli disc exits 0', r.returncode == 0, r.stderr)

# Let the viewport draw (and load the textures) first, then Ctrl+B.
ed_bin = os.path.join(tmp, 'editor.bin')
env = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe',
           SPLASHEDIT_DISC_ENGINE=engine)
show = '@menu.build/Show Disc Image in Folder'
notice_shot = os.path.join(tmp, 'notice.png')
cmd = [editor, '--size', '1600x960', '--project', project, '--export-to', ed_bin, '--wait', '30',
       '--click', '@menu.build', '--wait', '3', '--click', show, '--wait', '3', '--key', 'escape', '--wait', '3', '--key', 'ctrl+b', '--wait-play', '--wait', '5',
       '--shot', notice_shot, '--click', '@notice.reveal', '--wait', '3', '--click', '@menu.build', '--wait', '3',
       '--click', show, '--wait', '3', '--screenshot', os.path.join(tmp, 'shot.png')]
r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=120)
out = (r.stdout + r.stderr).strip()
check('editor exits 0 and writes the disc', r.returncode == 0 and os.path.isfile(ed_bin), out[-300:])
reveals = [l for l in r.stdout.splitlines() if l.startswith('reveal: ')]
check('Show in folder and the Build menu item each reveal the disc, the menu item not before the export',
      reveals == ['reveal: ' + ed_bin] * 2, reveals)
if os.environ.get('SHOT_DIR'):
    shutil.copy(notice_shot, os.path.join(os.environ['SHOT_DIR'], 'discreveal-notice.png'))
if fails:
    sys.exit(1)

want, got = iso_files(cli_bin), iso_files(ed_bin)
for name in ('SCENE0/SCENE_0.SPK;1', 'SCENE0/SCENE_0.VRM;1', 'SCENE0/SCENE_0.SPU;1'):
    a, b = want.get(name), got.get(name)
    diff = None if a is None or b is None or len(a) != len(b) else sum(x != y for x, y in zip(a, b))
    check(f'{name} from the editor matches splashpack-cli', a is not None and a == b,
          f'cli {None if a is None else len(a)} bytes, editor {None if b is None else len(b)} bytes, {diff} differ')

print(f'{fails} failures')
sys.exit(1 if fails else 0)
