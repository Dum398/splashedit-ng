# A skin clip's loop flag in the editor, driven through the GUI on a copy of
# tests/skin (bend.anim loops, raise.anim plays once) with an Agent on Arm
# whose Patrol plays raise. The Skinned animation section has a switch per
# clip (obj.skinloop.<i>) that rewrites the clip's .anim as one undo step, and
# the Baked row says per clip whether it "loops" or plays "once". The Agent
# section warns (obj.agonce<i>) when Idle, Patrol, Seek, Flee or Wander plays
# a clip that plays once; its fix (obj.agonce<i>.fix) makes the clip loop.
# Under scripted runs the editor prints, as they change,
#   skin: clip <name> frames <f> bytes <n> loops|once
#   agent: <name> plays once: <clips>|none
# usage: python3 cliploopcheck.py <splashedit> [shot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
fixture = os.path.join(here, 'skin')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
# Scrolls the inspector to its end, where the Agent section is.
DOWN = ['--wheel', '1420,600,-30'] + W(4)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'skin')
    shutil.copytree(fixture, project)
    path = os.path.join(project, 'skin.scene')
    s = json.load(open(path, encoding='utf-8'))
    s['objects'][0]['components'].append({'type': 'agent', 'stateClips': {'Patrol': 1}})
    json.dump(s, open(path, 'w', encoding='utf-8'), indent=1)
    return project


def run(name, args, shot=None):
    """Runs the editor on a fresh copy with Arm selected and its export done,
    then `args`, then waits for the export; returns the skin clip lines as
    {clip: (frames, loops)} after each block, the agent once lines, the named
    rects, raise.anim's loop on disk and the screenshot."""
    project = project_copy()
    shot = shot or os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--scene', os.path.join(project, 'skin.scene'),
           '--select', 'Arm'] + W(5) + ['--wait-export'] + args + W(3) + ['--wait-export'] + W(3) + \
          ['--move', '650,20'] + W(5) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l or 'still behind' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    clips = []
    for l in lines:
        if re.match(r'skin: Arm bones ', l): clips.append({})
        m = re.match(r'skin: clip (\S+) frames (\d+) bytes \d+ (loops|once)$', l)
        if m and clips: clips[-1][m.group(1)] = (int(m.group(2)), m.group(3))
    once = [l for l in lines if l.startswith('agent: Arm plays once: ')]
    loop = json.load(open(os.path.join(project, 'raise.anim'), encoding='utf-8'))['loop']
    return clips, once, rects, loop, shot


def inside(rects, name):
    r, p = rects.get(name), rects.get('obj.panel')
    return r is not None and p is not None and p[0] <= r[0] and r[2] <= p[2] and p[1] <= r[1] and r[3] <= p[3]


# 1. As the files say: bend loops (15 frames over [0, 1)), raise plays once
# (9 frames up to its end pose at 0.5 s); Patrol on raise is warned about.
clips, once, rects, loop, shot = run('base', [])
check('the Baked lines say bend loops and raise plays once',
      clips[-1:] == [{'bend': (15, 'loops'), 'raise': (9, 'once')}], clips)
check('a loop switch per clip, in the panel', inside(rects, 'obj.skinloop.0') and inside(rects, 'obj.skinloop.1'),
      (rects.get('obj.skinloop.0'), rects.get('obj.skinloop.1'), rects.get('obj.panel')))
check('the switches sit under Clips, above the Baked row',
      rects.get('obj.skinloop.0', (0, 0, 0, 0))[1] < rects.get('obj.skinloop.1', (0, 0, 0, 0))[1] <
      rects.get('obj.skininfo', (0, 0, 0, 0))[1], (rects.get('obj.skinloop.1'), rects.get('obj.skininfo')))
check('Patrol on raise prints the plays once line', once == ['agent: Arm plays once: raise'], once)
if keep: shutil.copyfile(shot, keep)

_, once, rects, _, shot = run('warned', DOWN)
check('the Agent section shows the plays once card for raise, in the panel', inside(rects, 'obj.agonce1') and
      inside(rects, 'obj.agonce1.fix'), (rects.get('obj.agonce1'), rects.get('obj.agonce1.fix'), rects.get('obj.panel')))
check('and none for bend', 'obj.agonce0' not in rects, sorted(k for k in rects if k.startswith('obj.agonce')))
if keep: shutil.copyfile(shot, os.path.splitext(keep)[0] + '-agent.png')

# 2. The switch makes raise loop: the file says so, the export bakes it as a
# loop (one frame fewer: no end pose) and the warning goes.
TOGGLE = ['--click', '@obj.skinloop.1'] + W(4)
clips, once, rects, loop, _ = run('toggle', TOGGLE + ['--wait-export'] + DOWN)
check('the switch writes loop true to raise.anim', loop is True, loop)
check('raise bakes again as a loop with another frame count',
      len(clips) >= 2 and clips[-1].get('raise', (0, ''))[1] == 'loops' and clips[-1]['raise'][0] != 9 and
      clips[-1].get('bend') == (15, 'loops'), clips)
check('the plays once line clears', once[-1:] == ['agent: Arm plays once: none'], once)
check('and the card goes', 'obj.agonce1' not in rects, sorted(k for k in rects if k.startswith('obj.agonce')))

# 3. Ctrl+Z puts the file back, and the bake and the warning with it.
clips, once, rects, loop, _ = run('undo', TOGGLE + ['--wait-export', '--key', 'ctrl+z'] + W(4) + ['--wait-export'] + DOWN)
check('Ctrl+Z writes loop false back', loop is False, loop)
check('raise bakes as once again, 9 frames', clips[-1:] and clips[-1].get('raise') == (9, 'once'), clips)
check('the plays once line is back', once[-1:] == ['agent: Arm plays once: raise'], once)
check('and so is the card', 'obj.agonce1' in rects, sorted(k for k in rects if k.startswith('obj.agonce')))

# 4. The card's fix makes the clip loop the same way.
clips, once, rects, loop, _ = run('fix', DOWN + ['--click', '@obj.agonce1.fix'] + W(4) + ['--wait-export'])
check('the fix writes loop true to raise.anim', loop is True, loop)
check('and the warning clears', once[-1:] == ['agent: Arm plays once: none'] and 'obj.agonce1' not in rects, (once, rects.get('obj.agonce1')))
check('raise bakes as a loop', clips[-1:] and clips[-1].get('raise', (0, ''))[1] == 'loops', clips)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
