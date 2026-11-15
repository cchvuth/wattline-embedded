#!/usr/bin/env python3
"""The dash's loop against tools/fake_panda.py over loopback, for the fake's scripted minute: what the UI is given
follows the drive (speed, gear, power, SoC, lamps, Autopilot, charging...), the link goes quiet, is lost and comes
back with a fresh subscribe, and every datagram the dash sent passed the listen-only gate (the fake checks each).
usage: test_app_bench.py build/app-host/dash-host        (make -f app.mk test)
PNGs of the screen every 5 s land in build/app-bench/."""
import json, os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
host_bin = sys.argv[1]
out = os.path.join(ROOT, 'build', 'app-bench')
os.makedirs(out, exist_ok=True)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 0))
port = s.getsockname()[1]
s.close()
conf = os.path.join(out, 'dash.conf')
open(conf, 'w').write(f'panda_host = 127.0.0.1\npanda_port = {port}\nlog = {out}/dash.log\n'
                      f'backlight = {out}\nset_clock = 0\ntz = UTC0\nhour12 = 1\n')
open(os.path.join(out, 'max_brightness'), 'w').write('100\n')
if os.path.exists(os.path.join(out, 'dash.log')):
    os.remove(os.path.join(out, 'dash.log'))

fake = subprocess.Popen([sys.executable, '-I', os.path.join(ROOT, 'tools', 'fake_panda.py'), '--bind', '127.0.0.1',
                         '--port', str(port), '--quiet'], stdout=subprocess.PIPE, text=True)
assert fake.stdout.readline().startswith('fake panda on')
dash = subprocess.run([host_bin, '-c', conf, '-t', '62', '-i', '100', '-p', out], capture_output=True, text=True)
fake.terminate()
fake_out = fake.communicate()[0]
summary = json.loads([l for l in fake_out.splitlines() if l.startswith('{"summary"')][-1])['summary']
rows = [json.loads(l) for l in dash.stdout.splitlines() if l.startswith('{')]
open(os.path.join(out, 'states.jsonl'), 'w').write(dash.stdout)
assert dash.returncode == 0, dash.stderr[-2000:]

fails = []
WIFI = dict(idle=0, hello=4, live=5, quiet=6, retrying=8)
AP = dict(none=0, ready=1, steering=2, tacc=3, hold=4)


def win(a, b):
    w = [r for r in rows if a <= r['t'] < b]
    assert w, (a, b)
    return w


def check(name, ok):
    print(('ok   ' if ok else 'FAIL ') + name)
    if not ok:
        fails.append(name)


def near(v, x, tol):
    return v is not None and abs(v - x) <= tol


w = win(1.5, 3.8)
check('parked: WiFi live, P, 0 km/h', all(r['wifi'] == WIFI['live'] and r['gear'] == 'P' and r['speed'] == 0 for r in w))
check('parked: SoC 78 %, range 240 mi in km', all(near(r['soc'], 78, 0.01) and near(r['range'], 240 * 1.609344, 0.01) for r in w))
check('parked: tyres 2.9 bar in psi, outside 12.5', all(near(r['tyre_fl'], 2.9 * 14.5038, 0.01) and near(r['outside'], 12.5, 0.01) for r in w))
check('parked: seat map in, driver buckled, DRL', all(r['cabin'] == 1 and r['seat_d'] == 1 and r['drl'] and not r['low'] for r in w))
check('parked: degradation 7.1 %, imbalance 4 mV', all(near(r['degradation'], (1 - 78 / 84) * 100, 0.01) and near(r['imbalance'], 4, 0.01) for r in w))
check('parked: ~50 speed frames/s, day backlight 100 %', all(45 <= r['speed_hz'] <= 55 and r['backlight'] == 100 for r in w))
check('parked: no regen cap in P', all(r['regen_cap'] is None for r in w))
w = win(11.5, 13.8)
check('accelerating: D, 70-100 km/h, rear 90 kW, front 40 kW, pedal 60', all(
    r['gear'] == 'D' and 70 <= r['speed'] <= 100 and near(r['rear_kw'], 90, 3) and near(r['front_kw'], 40, 3)
    and r['pedal'] == 60 for r in w))
check('accelerating: regen cap 60, Autopilot ready', all(near(r['regen_cap'], 60, 0.01) and r['ap'] == AP['ready'] for r in w))
w = win(16, 21.8)
check('cruise: 100 km/h, Autopilot steering, set 100, limit 100', all(
    r['speed'] == 100 and r['ap'] == AP['steering'] and r['cruise_set'] == 100 and r['limit'] == 100 for r in w))
w = win(17.3, 20.8)
check('cruise: the left indicator flashes (lit and dark both seen)', any(r['ind_l'] for r in w) and not all(r['ind_l'] for r in w))
check('cruise: hands-on nag at 22 s', all(r['ap'] == AP['hold'] for r in win(22.6, 23.8)))
w = win(25.5, 29.8)
check('braking: brake lamp, brake %, regen -45/-15 kW', all(
    r['braking'] and r['brake_pct'] > 0 and r['rear_kw'] < -30 and r['front_kw'] < -5 for r in w))
check('braking at night: low beams, backlight 30 %', all(r['low'] and r['backlight'] == 30 for r in w))
check('stopping: the right indicator flashes', any(r['ind_r'] for r in win(30.3, 33.3)))
w = win(34.5, 37)
check('parked again: P; the front left door (a polled group: up to 2 s late) open', all(r['gear'] == 'P' for r in w)
      and any(r['door_fl'] for r in w))
w = win(40.5, 45)
check('link loss: quiet (resubscribing), nothing live', any(r['wifi'] == WIFI['quiet'] for r in w) and not any(r['live'] for r in w))
check('link loss: the car\'s level held for bright_stale_ms (5 s)', all(r['backlight'] == 30 for r in win(40.5, 45)))
check('link loss: then dim to the idle 10 %', any(r['backlight'] == 10 for r in win(45, 48)))
check('link loss: given up and retried', any(r['wifi'] in (WIFI['retrying'], WIFI['hello']) for r in win(45, 48)))
w = win(49, 59.6)   # the fake's cycle starts over at 60
check('back: WiFi live again', all(r['wifi'] == WIFI['live'] for r in w))
check('charging: 11 kW, to 90 %, 95 min, port open', all(
    r['charging'] and near(r['charge_kw'], 11, 0.01) and r['limit_pct'] == 90 and r['minutes'] == 95 and r['charge_port']
    for r in w))
check('charging: done at 12:05 PM (0x318 clock 10:30:50 on, + 95 min)', all(r['charge_clock'] in ('12:05 PM', '12:06 PM') for r in w))
check('charging: the SoC climbs', w[-1]['soc'] > 78.3)

print('fake:', summary)
check('listen-only: every datagram passed the gate', summary['illegal'] == 0)
check('only ehllo / 0x18 / 0x0f / 0x0e, all of them used', all(summary[k] > 0 for k in ('ehllo', 'clear', 'add', 'remove')))
check('resubscribed after the loss: 3+ clears, 2+ sockets', summary['clear'] >= 3 and summary['clients'] >= 2)
log = open(os.path.join(out, 'dash.log')).read()
check('log: link states and stats written', 'wifi: live' in log and 'wifi: link lost' in log and 'frames/s' in log)
print(f'{len(rows)} states, {len(fails)} failed; PNGs in {out}')
sys.exit(1 if fails else 0)
