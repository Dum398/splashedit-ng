# Export tests/skin/skin.scene with agents on three objects and read the
# agent records back the way psxsplash's splashpack.cpp does: agentCount at
# header offset 114, then right after the interactable records (no alignment)
# agentCount x 28-byte SPLASHPACKAgentV2, then every agent's waypoints in the
# same order (waypointCount x 3 x i32). Every field is compared with the
# conversions in docs/splashpack-format.md 2.7, computed here on their own;
# waypoint Y is negated like every other engine position. Also: the warnings
# (more than 8 waypoints, field of view outside (0, 360], state clips without a
# skin or past its clips, a held state on a clip that plays once, no nav
# regions), the `--stats` agent lines with the clips written, the game object each record names
# (through the object name table), and resave keeping the component.
# usage: python3 agentcheck.py <splashpack-cli>
import json, math, os, shutil, struct, subprocess, sys, tempfile

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp()
fails = checks = 0
GTE = 100.0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def obj(name, comps, pos=(0, 0, 0), active=True, rot=(0, 0, 0, 1)):
    return {'name': name, 'active': active, 'transform': {'position': list(pos), 'rotation': list(rot), 'scale': [1, 1, 1]},
            'components': comps, 'children': []}


ARM = {'type': 'agent', 'moveSpeed': 2.5, 'stopDistance': 0.75, 'visionRange': 6, 'fov': 120, 'visionRegionDepth': 3,
       'hearingRange': 3, 'alertTimeout': 120, 'waypoints': [[1, 0, 2], [-2.5, 1.5, 4], [0.333, -0.25, -7.125]],
       'stateClips': {'Idle': 1, 'Patrol': 0, 'Attack': 5}}
GHOST = {'type': 'agent', 'startEnabled': False, 'vision': False, 'waypoints': [[i, 0.5, -i] for i in range(10)]}
STRAY = {'type': 'agent', 'fov': 400, 'visionRange': 0, 'hearingRange': 0, 'stateClips': {'Flee': 0}}


def scene():
    s = json.load(open(os.path.join(here, 'skin', 'skin.scene')))
    s['settings']['script'] = os.path.join(here, 'skin', s['settings']['script'])
    arm = s['objects'][0]
    for c in arm['components']:
        if c['type'] == 'mesh':
            c['mesh'] = os.path.join(here, 'skin', c['mesh'])
        if c['type'] == 'skin':
            c['clips'] = [os.path.join(here, 'skin', p) for p in c['clips']]
    arm['components'].append(ARM)
    s['objects'] += [obj('Spacer', []), obj('Ghost', [GHOST], (3, 0, 1)), obj('Off', [dict(GHOST)], active=False),
                     obj('Stray', [STRAY])]
    return json.loads(json.dumps(s))  # nothing shared with ARM, GHOST, STRAY


def export(s, name='a'):
    path = os.path.join(tmp, name + '.scene')
    json.dump(s, open(path, 'w'))
    out = os.path.join(tmp, name + '.splashpack')
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([cli, 'export', path, '-o', out, '--stats'], capture_output=True, text=True)
    return r, (open(out, 'rb').read() if r.returncode == 0 else b'')


def decode(d):
    version, nlua, ngo = struct.unpack_from('<HHH', d, 2)
    ncol, nint = struct.unpack_from('<HH', d, 12)
    nodes, refs = struct.unpack_from('<HH', d, 32)
    ntrig, = struct.unpack_from('<H', d, 38)
    nametab, = struct.unpack_from('<I', d, 60)
    nagent, = struct.unpack_from('<H', d, 114)
    p = (156 if version >= 25 else 148) + nlua * 8 + ngo * 92 + ncol * 32 + ntrig * 32
    p = (p + 3) & ~3
    p += nodes * 32 + refs * 4
    p = (p + 3) & ~3
    p += nint * 32
    agents = []
    for i in range(nagent):
        f = struct.unpack_from('<HBBHHHhHH8sB3s', d, p)
        agents.append(dict(go=f[0], flags=f[1], nwp=f[2], speed=f[3], stop=f[4], vrange=f[5], vcos=f[6], hear=f[7],
                           alert=f[8], clips=list(f[9]), depth=f[10], reserved=f[11]))
        p += 28
    for a in agents:
        a['wps'] = [struct.unpack_from('<iii', d, p + 12 * k) for k in range(a['nwp'])]
        p += 12 * a['nwp']
    names, q = [], nametab
    for i in range(ngo):
        n = d[q]
        names.append(d[q + 1:q + 1 + n].decode('utf-8'))
        q += n + 2
    return version, agents, names, p


def rnd(x):
    """C#'s Mathf.RoundToInt: round half to even, like nearbyint and Python's round."""
    return int(round(x))


def expect(c, go):
    """The record psxsplash should read for agent component `c` (defaults filled in), per section 2.7."""
    full = {'startEnabled': True, 'moveSpeed': 2, 'stopDistance': 0.5, 'vision': True, 'visionRange': 8, 'fov': 90,
            'visionRegionDepth': 2, 'hearingRange': 0, 'alertTimeout': 90, 'waypoints': [], 'stateClips': {}}
    full.update(c)
    n = min(len(full['waypoints']), 8)
    flags = (1 if full['startEnabled'] else 0) | (2 if full['vision'] and full['visionRange'] > 0 else 0) | \
            (4 if full['hearingRange'] > 0 else 0) | (8 if n > 0 else 0)
    fov = min(max(full['fov'], 0), 360)
    states = ['Idle', 'Patrol', 'Seek', 'Flee', 'Attack', 'Wander', 'Investigate', 'Custom']
    clips = [full['stateClips'].get(s, 0xFF) for s in states]
    clips = [v if v != 0xFF and v < go[1] else 0xFF for v in clips]  # go[1]: the object's skin clip count
    fp = lambda u: min(max(rnd(u / GTE * 4096), 0), 65535)
    return dict(go=go[0], flags=flags, nwp=n, speed=min(max(rnd(full['moveSpeed'] / 30 / GTE * 4096), 0), 65535),
                stop=fp(full['stopDistance']), vrange=fp(full['visionRange']),
                vcos=min(max(rnd(math.cos(math.radians(fov / 2)) * 4096), -4096), 4096), hear=fp(full['hearingRange']),
                alert=full['alertTimeout'], clips=clips, depth=full['visionRegionDepth'], reserved=b'\0\0\0',
                wps=[(rnd(x / GTE * 4096), rnd(-y / GTE * 4096), rnd(z / GTE * 4096)) for x, y, z in full['waypoints'][:8]])


r, d = export(scene())
check('export', r.returncode == 0, r.stderr)
version, agents, names, end = decode(d)
check('three agents (the inactive one left out)', len(agents) == 3, len(agents))
by = {names[a['go']] if a['go'] < len(names) else '?': a for a in agents}
for k in ('Arm', 'Ghost', 'Stray'):  # a missing record fails its checks below instead of stopping the run
    by.setdefault(k, {'nwp': None, 'wps': [(None,) * 3] * 2, 'clips': [None] * 8, 'vcos': None, 'flags': None})
check('records name Arm, Ghost and Stray in scene order',
      [names[a['go']] if a['go'] < len(names) else '?' for a in agents] == ['Arm', 'Ghost', 'Stray'],
      [a['go'] for a in agents])
check('the mesh-less agents are game objects, the empty Spacer is not', 'Ghost' in names and 'Stray' in names and
      'Spacer' not in names, names)
for nm, comp, clips in (('Arm', ARM, 2), ('Ghost', GHOST, 0), ('Stray', STRAY, 0)):
    want = expect(comp, (names.index(nm) if nm in names else -1, clips))
    got = by.get(nm, {})
    for k in want:
        check('%s %s' % (nm, k), got.get(k) == want[k], (got.get(k), want[k]))
check('Arm: Patrol plays clip 0, Idle clip 1, Attack (clip 6 of 2) none',
      by['Arm']['clips'][:2] == [1, 0] and by['Arm']['clips'][4] == 0xFF, by['Arm']['clips'])
check('Arm: waypoint Y is negated (1.5 m up -> -61)', by['Arm']['wps'][1][1] == -61, by['Arm']['wps'])
check('Ghost: 10 waypoints, 8 written', by['Ghost']['nwp'] == 8 and len(by['Ghost']['wps']) == 8, by['Ghost']['nwp'])
check('Stray: fov 400 written as 360 (cosine -4096)', by['Stray']['vcos'] == -4096, by['Stray']['vcos'])
check('Stray: no vision, hearing or patrol bits', by['Stray']['flags'] == 1, by['Stray']['flags'])
nav = struct.unpack_from('<H', d, 44)[0]
check('no nav regions here, and the waypoints end where the next block begins (nav/rooms none: atlas metadata)',
      nav == 0 and end <= len(d), (nav, end, len(d)))
err = r.stderr
check('warns: Ghost has 10 waypoints', "Ghost: agent has 10 waypoints" in err, err)
check('warns: Stray field of view', "Stray: agent field of view" in err, err)
check('warns: Stray plays a clip without a skin', "Stray: agent plays a clip in state Flee but the object has no skin" in err, err)
check('warns: Arm Attack clip past the skin', "Arm: agent state Attack plays clip 6 of a skin with 2" in err, err)
check('warns: Arm Idle plays raise, which plays once',
      "Arm: agent state Idle plays clip raise, which plays once; it stops on its last frame" in err, err)
check('no plays-once warning for Patrol on bend (it loops) or Attack (past the skin)',
      err.count('which plays once') == 1, err)
check('warns: agents without nav regions', "The agents can't move: the scene has no nav regions" in err, err)
check('the inactive agent is not warned about', 'Off:' not in err, err)
stats = [l for l in r.stdout.splitlines() if l.startswith('agent ')]
check('--stats lists each agent with its bytes and the clips written (Attack past the skin, Stray without one: none)',
      stats == ['agent object=0 waypoints=3 bytes=64 clips=Idle:1,Patrol:0 Arm',
                'agent object=1 waypoints=8 bytes=124 clips=- Ghost', 'agent object=2 waypoints=0 bytes=28 clips=- Stray'], stats)

# No agents: the count is 0 and nothing is warned.
s0 = scene()
for o in s0['objects']:
    o['components'] = [c for c in o['components'] if c['type'] != 'agent']
r0, d0 = export(s0, 'none')
check('without agents agentCount is 0', r0.returncode == 0 and struct.unpack_from('<H', d0, 114)[0] == 0, r0.stderr)
check('without agents no agent warning', 'agent' not in r0.stderr.lower(), r0.stderr)

# The courtyard has a player and a floor: the bake gives regions, so no nav warning.
cy = os.path.join(tmp, 'courtyard')
shutil.copytree(os.path.join(here, '..', 'examples', 'courtyard'), cy)
sc = json.load(open(os.path.join(cy, 'courtyard.scene')))
sc['objects'].append(obj('Guard', [{'type': 'agent', 'waypoints': [[-3, 0, 3], [2, 0, 3]]}], (-1, 0, 1)))
json.dump(sc, open(os.path.join(cy, 'courtyard.scene'), 'w'))
rc = subprocess.run([cli, 'export', os.path.join(cy, 'courtyard.scene'), '-o', os.path.join(cy, 'c.splashpack')],
                    capture_output=True, text=True)
dc = open(os.path.join(cy, 'c.splashpack'), 'rb').read() if rc.returncode == 0 else b''
check('courtyard export with a Guard', rc.returncode == 0 and struct.unpack_from('<H', dc, 44)[0] > 0, rc.stderr)
check('with nav regions there is no nav warning', "can't move" not in rc.stderr, rc.stderr)

# Bad values in the scene file are refused; resave keeps the component.
for bad, word in (({'visionRegionDepth': 9}, 'visionRegionDepth'), ({'alertTimeout': 70000}, 'alertTimeout'),
                  ({'stateClips': {'Sleep': 0}}, 'Sleep'), ({'moveSpeed': -1}, 'ranges')):
    sb = scene()
    sb['objects'][-1]['components'][0].update(bad)
    rb, _ = export(sb, 'bad')
    check('refused: %s' % json.dumps(bad), rb.returncode != 0 and word in rb.stderr, (rb.returncode, rb.stderr))
src, dst = os.path.join(tmp, 'a.scene'), os.path.join(tmp, 'rs.scene')
rr = subprocess.run([cli, 'resave', src, dst], capture_output=True, text=True)
saved = json.load(open(dst)) if rr.returncode == 0 else {}
comp = [c for o in saved.get('objects', []) if o['name'] == 'Arm' for c in o['components'] if c['type'] == 'agent']
check('resave keeps the Arm agent', comp and comp[0]['waypoints'] == ARM['waypoints'] and
      comp[0]['stateClips'] == ARM['stateClips'] and comp[0]['fov'] == 120 and comp[0]['alertTimeout'] == 120, (rr.stderr, comp))

print('%d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
