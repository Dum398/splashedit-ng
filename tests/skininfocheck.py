# What the object inspector says about a skinned mesh, driven through the GUI.
# Under the Bake rate the Skinned animation section shows a Baked row
# (obj.skininfo): the bones and triangles, one line per clip with its frames,
# size and whether it loops, and the whole skin's size, all from the last live
# export. The row's height is fixed by the clip list from the first frame, so
# nothing below it moves when the export lands. A clip that bakes more than
# 1000 frames adds a "Long clip" card (obj.skinlong). Under scripted runs the
# editor prints "skin: <object> bones <b> clips <c> bytes <n>" and
# "skin: clip <name> frames <f> bytes <n> loops|once" per clip as they change.
# usage: python3 skininfocheck.py <splashedit> [shot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
fixture = os.path.join(here, 'skin')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy(clip_length=None):
    """tests/skin in a temp dir; clip_length stretches bend.anim to that many
    seconds (its keys scaled with it)."""
    project = os.path.join(tempfile.mkdtemp(), 'skin')
    shutil.copytree(fixture, project)
    if clip_length:
        path = os.path.join(project, 'bend.anim')
        a = json.load(open(path, encoding='utf-8'))
        k = clip_length / a['length']
        a['length'] = clip_length
        for ch in a['channels']:
            ch['times'] = [t * k for t in ch['times']]
        json.dump(a, open(path, 'w', encoding='utf-8'), indent=1)
    return project


def run(args, shot=None, wait_export=True, env=None, clip_length=None):
    """Runs the editor on a fresh copy with Arm selected, then `args`; returns
    the printed skin: lines, in order, and the named controls' rects at the end."""
    project = project_copy(clip_length)
    cmd = [editor, '--size', '1600x960', '--project', project, '--scene', os.path.join(project, 'skin.scene'),
           '--select', 'Arm', '--wait', '5'] + (['--wait-export'] if wait_export else []) + args + \
          ['--wait', '3', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(ENV, **(env or {})), timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check('every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return [l for l in lines if l.startswith('skin: ')], rects


def inside(rects, name):
    r, p = rects.get(name), rects.get('obj.panel')
    return r is not None and p is not None and p[0] <= r[0] and r[2] <= p[2] and p[1] <= r[1] and r[3] <= p[3]


loops = {}  # clip: loops or once, from the last parse


def parse(lines):
    """The last skin block printed: (bones, bytes, {clip: (frames, bytes)})."""
    head, clips = None, {}
    loops.clear()
    for l in lines:
        m = re.match(r'skin: Arm bones (\d+) clips (\d+) bytes (\d+)$', l)
        if m:
            head, clips = (int(m.group(1)), int(m.group(3))), {}
            continue
        m = re.match(r'skin: clip (\S+) frames (\d+) bytes (\d+) (loops|once)$', l)
        if m:
            clips[m.group(1)] = (int(m.group(2)), int(m.group(3)))
            loops[m.group(1)] = m.group(4)
    return head, clips


tmp = tempfile.mkdtemp()
shot = os.path.join(tmp, 'skin.png')

# At 15 fps: two bones, both clips, sizes that add up, the row in the panel.
lines, rects = run([], shot=shot)
head, clips = parse(lines)
check('Arm prints its skin: 2 bones, 2 clips', head is not None and head[0] == 2 and len(lines) == 3 and
      lines[0].startswith('skin: Arm bones 2 clips 2 bytes '), lines)
check('each clip prints frames and bytes', sorted(clips) == ['bend', 'raise'] and all(f > 0 and b > 0 for f, b in clips.values()), clips)
check('and whether it loops, as its file says', loops == {'bend': 'loops', 'raise': 'once'}, loops)
check('the clips fit in the skin', head is not None and len(clips) == 2 and sum(b for _, b in clips.values()) < head[1], (head, clips))
check('the Baked row lies in the inspector panel, under Bake rate', inside(rects, 'obj.skininfo') and
      rects['obj.skininfo'][1] >= rects.get('s_skin', (0, 0, 0, 1 << 30))[3] + 64, (rects.get('s_skin'), rects.get('obj.skininfo'), rects.get('obj.panel')))
check('short clips show no Long clip card', 'obj.skinlong' not in rects, sorted(k for k in rects if k.startswith('obj.skin')))
if keep: shutil.copyfile(shot, keep)

# Before the export lands the row already has its full height: same rect,
# no skin: lines (the export is held back 60 s).
early, early_rects = run([], wait_export=False, env={'SPLASHEDIT_EXPORT_DELAY_MS': '60000'})
check('before the export there are no skin lines', early == [], early)
check('and the Baked row is already its final size', early_rects.get('obj.skininfo') == rects.get('obj.skininfo'),
      (early_rects.get('obj.skininfo'), rects.get('obj.skininfo')))

# 30 fps: every clip bakes more frames and takes more bytes, and so does the skin.
lines30, _ = run(['--dblclick', '@obj.sfps', '--wait', '3', '--key', 'ctrl+a', '--text', '30', '--key', 'enter', '--wait', '3',
                  '--wait-export'])
head30, clips30 = parse(lines30)
check('30 fps prints the 15 fps skin, then a bigger one', lines30[:3] == lines and head30 is not None and head is not None and
      head30[1] > head[1], (lines30, lines))
check('every clip grows in frames and bytes', sorted(clips30) == sorted(clips) == ['bend', 'raise'] and
      all(clips30[c][0] > clips[c][0] and clips30[c][1] > clips[c][1] for c in clips), (clips, clips30))

# A 50 s clip at 30 fps bakes 1500 frames: the Long clip card shows, in the panel.
lines, rects = run(['--dblclick', '@obj.sfps', '--wait', '3', '--key', 'ctrl+a', '--text', '30', '--key', 'enter', '--wait', '3',
                    '--wait-export'], clip_length=50)
_, clips = parse(lines)
check('a 50 s clip at 30 fps bakes over 1000 frames', clips.get('bend', (0, 0))[0] > 1000, clips)
check('and shows the Long clip card in the panel', inside(rects, 'obj.skinlong'), (rects.get('obj.skinlong'), rects.get('obj.panel')))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
