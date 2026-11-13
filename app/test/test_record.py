#!/usr/bin/env python3
"""Drive recording end to end: dash-host with "Record drives" on (settings.conf record = 1) records the fake's scripted
drive over loopback; the recording is then streamed by fake_panda.py --replay into a second dash-host (recording off),
and the speed / gear / power it derives follow the first run's. Also: only subscribed ids are written, the session is
named by the clock, record_max_mb deletes the oldest session first, and nothing is written with recording off.
usage: test_record.py build/app-host/dash-host        (make -f app.mk test)"""
import json, os, re, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.dont_write_bytecode = True   # no tools/__pycache__ in the tree
import fake_panda

host_bin = sys.argv[1]
out = os.path.join(ROOT, 'build', 'app-record')
shutil.rmtree(out, ignore_errors=True)
SECS = 36   # parked, accelerate, cruise, brake, stop, parked with a door open
fails = []


def check(name, ok):
    print(('ok   ' if ok else 'FAIL ') + name)
    if not ok:
        fails.append(name)


def run(tag, record, fake_args):
    data = os.path.join(out, tag)
    os.makedirs(data)
    open(os.path.join(data, 'settings.conf'), 'w').write(f'record = {int(record)}\n')
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    conf = os.path.join(data, 'dash.conf')
    open(conf, 'w').write(f'panda_host = 127.0.0.1\npanda_port = {port}\nlog = {data}/dash.log\nbacklight =\n'
                          f'set_clock = 0\ntz = UTC0\nrecord_max_mb = 1\n')
    if record:   # two older sessions: 0.9 MB, so the cap of 1 MB takes the oldest once this one passes 0.1 MB
        os.makedirs(os.path.join(data, 'rec'))
        for name, kb, age in (('old1', 600, 7200), ('old2', 300, 3600)):
            p = os.path.join(data, 'rec', name + '.panda')
            open(p, 'wb').write(b'\0' * kb * 1024)
            os.utime(p, (time.time() - age,) * 2)
    fake = subprocess.Popen([sys.executable, '-I', os.path.join(ROOT, 'tools', 'fake_panda.py'), '--bind', '127.0.0.1',
                             '--port', str(port), '--quiet'] + fake_args, stdout=subprocess.PIPE, text=True)
    assert fake.stdout.readline().startswith('fake panda on')
    dash = subprocess.run([host_bin, '-c', conf, '-t', str(SECS), '-i', '100', '-s', data], capture_output=True, text=True)
    fake.terminate()
    fake.communicate()
    assert dash.returncode == 0, dash.stderr[-2000:]
    return data, [json.loads(l) for l in dash.stdout.splitlines() if l.startswith('{')]


data, a = run('record', True, [])
rec = os.path.join(data, 'rec')
sessions = sorted(f for f in os.listdir(rec) if re.fullmatch(r'\d{8}-\d{6}\.panda', f))
check('one session, named YYYYMMDD-HHMMSS by the clock', len(sessions) == 1)
check('record_max_mb: the oldest session deleted, the next kept', not os.path.exists(os.path.join(rec, 'old1.panda'))
      and os.path.exists(os.path.join(rec, 'old2.panda')))
check('no .cmdr without Commander pushes', not any(f.endswith('.cmdr') for f in os.listdir(rec)))
path = os.path.join(rec, sessions[0])
drive = fake_panda.load_replay(path)
src = open(os.path.join(ROOT, 'build', 'signals_gen.h')).read()
subscribed = {int(x, 16) for x in re.search(r'kSubscribeIds\[\] = \{([^}]*)\}', src).group(1).replace('0x', '').split(',')}
ids = {int.from_bytes(d[o:o + 4], 'little') >> 21 for _, d in drive for o in range(0, len(d), 16)}
check(f'{len(drive)} datagrams over {drive[-1][0]:.1f} s, only subscribed ids ({len(ids)}), no ACK',
      len(drive) > 1000 and drive[-1][0] > SECS - 3 and ids <= subscribed and 6 not in ids
      and all(len(d) % 16 == 0 for _, d in drive))
check('a time per datagram, never going back', all(t1 <= t2 for (t1, _), (t2, _) in zip(drive, drive[1:])))

data2, b = run('replay', False, ['--replay', path])
check('recording off: nothing written', not os.path.exists(os.path.join(data2, 'rec')))


def gears(rows):
    g = [r['gear'] for r in rows if r['gear'] != '-']
    return [x for i, x in enumerate(g) if i == 0 or g[i - 1] != x]


check(f'gear sequence the same: {gears(a)}', gears(a) == gears(b) == ['P', 'D', 'P'])
ta = next(r['t'] for r in a if r['gear'] == 'D')
tb = next(r['t'] for r in b if r['gear'] == 'D')
pairs = [(r, min(b, key=lambda x: abs(x['t'] - tb - (r['t'] - ta)))) for r in a if -2 <= r['t'] - ta <= SECS - 8]
same = lambda k, tol: sum(1 for r, x in pairs if r[k] is not None and x[k] is not None and abs(r[k] - x[k]) <= tol)
n = len(pairs)
check(f'speed follows the recording ({same("speed", 3)}/{n} within 3 km/h)', same('speed', 3) >= 0.95 * n)
check(f'rear power follows ({same("rear_kw", 5)}/{n} within 5 kW)', same('rear_kw', 5) >= 0.95 * n)
check(f'gear follows ({sum(r["gear"] == x["gear"] for r, x in pairs)}/{n})', sum(r['gear'] == x['gear'] for r, x in pairs) >= 0.95 * n)
check('the replay reaches 100 km/h and stops', max(r['speed'] for r in b) == 100 and b[-1]['speed'] == 0)
print(f'{len(fails)} failed; recording {path}')
sys.exit(1 if fails else 0)
