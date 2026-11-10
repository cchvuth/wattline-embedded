#!/usr/bin/env python3
"""Bench stand-in for the Commander's Panda UDP interface (lib/panda.dart's peer), stdlib only.

Answers "ehllo" and the filter ops (0x18 clear, 0x0f add, 0x0e remove) with the Commander's ACK record
(bus 15, id 6), and streams 16-byte records only for the subscribed ids, each at a realistic rate. The values
come from a scripted 60 s drive, ENCODED into frames with the app's signal table (build/signals_gen.h, written
by tools/gen_signals.py from lib/signals.dart + tools/extra_signals.json):

   0-4 s  parked          4-14 accelerate to 100 km/h   14-24 cruise on Autopilot (left signal 17-21, nag 22-24)
  24-30 brake + regen at night (low beams)   30-34 right signal, stop, P   34-37 parked, front left door open
  37-47 SILENCE (nothing sent, nothing answered: quiet, lost, retry)       47-60 charging at 11 kW, then again

The clock starts at the first id list (0x0f) received. Every datagram received is checked against the dash's
listen-only gate (core/panda.c panda_out_ok); anything else counts as illegal. A summary line (JSON) is printed
at exit (SIGINT/SIGTERM or --duration).

usage: fake_panda.py [--bind 0.0.0.0] [--port 1338] [--duration s] [--once] [--quiet]
"""
import argparse, json, os, re, select, signal, socket, struct, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CYCLE = 60.0
SILENT = (37.0, 47.0)

# Frames a second per id (DBC cycle times / what a capture shows); others DEFAULT_HZ.
RATES = {0x257: 50, 0x118: 100, 0x101: 100, 0x129: 100, 0x132: 100, 0x185: 50, 0x2E5: 2, 0x266: 2, 0x3F5: 10,
         0x399: 4, 0x33A: 1, 0x332: 2, 0x352: 1, 0x392: 1, 0x2B3: 10, 0x243: 4, 0x321: 1, 0x2F3: 2, 0x219: 2,
         0x25A: 1, 0x318: 4, 0x212: 10, 0x252: 10, 0x286: 10, 0x3F8: 10, 0x2B6: 10, 0x2A7: 10, 0x2B4: 10,
         0x3F6: 10, 0x102: 10, 0x103: 10, 0x273: 10, 0x3E2: 2, 0x2E1: 10, 0x25D: 10, 0x3A1: 10, 0x3C2: 10,
         0x31A: 10, 0x347: 1, 0x238: 1, 0x3FD: 1, 0x472: 1, 0x333: 1, 0x33B: 1}
DEFAULT_HZ = 5
FRAME_LEN = {0x219: 7, 0x3E2: 7}   # the decoders take these only at this length
BUS = {0x132: 1}                    # the capture's one bus-1 id


def load_signals():
    h = os.path.join(ROOT, 'build', 'signals_gen.h')
    if not os.path.exists(h):
        subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'gen_signals.py'),
                        os.path.join(ROOT, '..', 'tesla-dash'), os.path.join(ROOT, 'build')], check=True)
    src = open(h).read()
    src = src[src.index('kSignals[] = {'):src.index('};', src.index('kSignals[] = {'))]
    rows = re.findall(r'\{"(\w+)", 0x([0-9A-F]+), (\d+), (\d+), ([-\d.e]+), ([-\d.e]+), (\d), (-?\d+), (-?\d+), (\d+), (\d+)\}', src)
    assert len(rows) == src.count('{"'), 'signals_gen.h rows not all parsed'
    return {n: dict(id=int(i, 16), start=int(st), len=int(ln), scale=float(sc), offset=float(of), signed=sg == '1',
                    sna=int(sna), mux=int(mx), mux_start=int(ms), mux_len=int(ml))
            for n, i, st, ln, sc, of, sg, sna, mx, ms, ml in rows}


def put_bits(buf, start, length, raw):   # Intel (DBC @1) order, as signals.dart _bits()
    for i in range(length):
        b = start + i
        if raw >> i & 1:
            buf[b >> 3] |= 1 << (b & 7)
        else:
            buf[b >> 3] &= ~(1 << (b & 7))


def raw_of(s, v):
    raw = round((v - s['offset']) / s['scale'])
    n = s['len']
    lo, hi = (-(1 << n - 1), (1 << n - 1) - 1) if s['signed'] else (0, (1 << n) - 1)
    return max(lo, min(hi, raw)) & ((1 << n) - 1)


def out_ok(d):   # core/panda.c panda_out_ok, the dash's listen-only gate
    n = len(d)
    if n == 5:
        return d == b'ehllo'
    if n == 1:
        return d[0] == 0x18
    if n < 4 or (n - 1) % 3 or (n - 1) // 3 > 43 or d[0] not in (0x0f, 0x0e):
        return False
    return all(d[o] == 0xff and (d[o + 1] << 8 | d[o + 2]) <= 0x7ff for o in range(1, n, 3))


def record(cid, bus, data):
    return struct.pack('<II', cid << 21, bus << 4 | len(data)) + bytes(data).ljust(8, b'\0')


ACK = record(6, 15, b'\0')


def ramp(t, t0, t1, a, b):
    return a + (b - a) * min(1.0, max(0.0, (t - t0) / (t1 - t0)))


def blink(t, t0):   # the flasher: 2 lit, 1 on but dark; 370 ms halves
    return 2 if int((t - t0) * 1000 / 370) % 2 == 0 else 1


def scene(t):
    """(phase, {signal: value}) at t s into the cycle."""
    clock = 10 * 3600 + 30 * 60 + int(t)   # 2026-11-13 10:30:00 UTC on
    v = dict(utcYear=2026, utcMonth=11, utcDay=13, utcHour=clock // 3600, utcMinutes=clock // 60 % 60,
             utcSeconds=clock % 60,
             gear=1, speed=0, speedIsKph=1, pedal=0, accel=0, yaw=0, brake=0, steerSccm=0,
             brakeFL=0, brakeFR=0, brakeRL=0, brakeRR=0, frontKw=0, rearKw=0, packV=380, packA=-2.6,
             soc=78, rangeMi=240, cellVMax=3.902, cellVMin=3.898, packTMin=22, packKwh=78, packLeftKwh=60,
             packBufferKwh=3.5, packNewKwh=84, regenMax=0, motorHeat=0, pumpHeat=0,
             lowBeam=1, highBeam=0, highLamp=0, indL=0, indR=0, blindL=0, blindR=0, sideWarn=0,
             outside=12.5, washer=0, slippery=0, tcFlash=0, escFlash=0, tcMode=0,
             tpFL=2.9, tpFR=2.9, tpRL=2.95, tpRR=2.9, tdFL=2.9, tdFR=2.9, tdRL=2.95, tdRR=2.9,
             doorFL=2, doorRL=2, doorFR=2, doorRR=2, trunk=2, frunk=2, mirrorDipped=0, tiltAuto=1, chargePort=0,
             domeLight=2, mapLights=0,
             beltDriver=0, beltPass=0, belt2L=0, belt2C=0, belt2R=0, belt3L=0, belt3R=0,
             buckleD=2, buckle2L=0, buckle2C=0, buckleP=0, buckle2R=0, buckle3L=0, buckle3R=0,
             hvacPower=1, recirc=0, recircPct=0, climateOn=1, autoBlower=2, fan1=11, fan2=0,
             cabin1=21, cabin2=20, cabin3=19, co2=650, heatL=0, heatR=0, compV=380, compW=800, lvV=14.5, lvA=40,
             speedLimit=0, autopilot=0, handsOn=0, cruiseState=0, cruiseSet=0, cruiseKph=1, followGap=2,
             navRoute=0, navMinutes=1023, navEnergyKwh=0, precondState=0, precondTarget=0,
             chargeStatus=0, chargeKw=0, chargeMaxKw=0, chargeWhy=0, chargeLimit=90, chargeMinutes=2047)
    kw = lambda total: -total * 1000 / v['packV']   # packA for a pack draw (pack kW = -V * A / 1000)
    if t < 4:
        return 'parked', v
    if t < 14:
        v.update(gear=4, speed=ramp(t, 4, 14, 0, 100), pedal=60, accel=2.8, rearKw=90, frontKw=40, packA=kw(133),
                 regenMax=60, autopilot=2)
        return 'accelerate', v
    if t < 24:
        v.update(gear=4, speed=100, pedal=18, rearKw=12, frontKw=5, packA=kw(19), regenMax=60, autopilot=3,
                 cruiseState=2, cruiseSet=100, speedLimit=100, handsOn=3 if t >= 22 else 0)
        if 17 <= t < 21:
            v['indL'] = blink(t, 17)
        return 'cruise', v
    if t < 30:
        v.update(gear=4, speed=ramp(t, 24, 30, 100, 30), brake=1, accel=-3.2, rearKw=-45, frontKw=-15,
                 packA=kw(-62), regenMax=60, autopilot=2, lowBeam=4, speedLimit=50,
                 brakeFL=600, brakeFR=600, brakeRL=300, brakeRR=300)
        return 'brake', v
    if t < 34:
        stopped = t >= 33.5
        v.update(gear=4, speed=ramp(t, 30, 33.5, 30, 0), brake=1, rearKw=0 if stopped else -10,
                 frontKw=0 if stopped else -3, packA=kw(-12), regenMax=0 if stopped else 60, lowBeam=4,
                 indR=0 if stopped else blink(t, 30), brakeFL=800, brakeFR=800, brakeRL=400, brakeRR=400)
        return 'stop', v
    if t < SILENT[0]:
        v.update(lowBeam=4, doorFL=1 if t >= 35 else 2)
        return 'parked', v
    if t < SILENT[1]:
        return 'SILENT', v
    v.update(chargeStatus=3, chargeKw=11.0, chargeMaxKw=11.5, chargeMinutes=95, chargePort=1, packA=kw(-11),
             soc=78 + (t - SILENT[1]) * 0.05)
    return 'charging', v


class Panda:
    def __init__(self, sigs, quiet):
        self.sigs, self.quiet = sigs, quiet
        self.by_id = {}
        for n, s in sigs.items():
            self.by_id.setdefault(s['id'], []).append(n)
        self.pages = {i: sorted({sigs[n]['mux'] for n in ns if sigs[n]['mux'] >= 0}) or [None]
                      for i, ns in self.by_id.items()}
        self.page_at = {i: 0 for i in self.by_id}
        self.subs, self.due = set(), {}
        self.client, self.clients = None, set()
        self.t0 = None
        self.counts = dict(ehllo=0, clear=0, add=0, remove=0, illegal=0, acks=0, datagrams=0, frames=0)
        self.illegal = []
        self.sent_ids = {}

    def frame(self, cid, values):
        pages = self.pages[cid]
        page = pages[self.page_at[cid] % len(pages)]
        self.page_at[cid] += 1
        buf = bytearray(FRAME_LEN.get(cid, 8))
        for n in self.by_id[cid]:
            s = self.sigs[n]
            if s['mux'] >= 0 and s['mux'] != page:
                continue
            if s['mux'] >= 0:
                put_bits(buf, s['mux_start'], s['mux_len'], page)
            if n in values:
                put_bits(buf, s['start'], s['len'], raw_of(s, values[n]))
        return record(cid, BUS.get(cid, 0), buf)

    def t(self, now):
        return None if self.t0 is None else now - self.t0

    def silent(self, now):
        t = self.t(now)
        return t is not None and SILENT[0] <= t % CYCLE < SILENT[1]

    def on_datagram(self, sock, d, addr, now):
        ok = out_ok(d)
        if not ok:
            self.counts['illegal'] += 1
            self.illegal.append(d.hex())
            print(f'ILLEGAL datagram from {addr}: {d.hex()}', flush=True)
            return
        kind = 'ehllo' if len(d) == 5 else 'clear' if d[0] == 0x18 else 'add' if d[0] == 0x0f else 'remove'
        self.counts[kind] += 1
        if self.silent(now):
            return   # the Commander gone: nothing heard, nothing answered
        if addr != self.client:
            self.clients.add(addr)
            if not self.quiet:
                print(f'client {addr[0]}:{addr[1]}', flush=True)
        self.client = addr
        ids = [d[o + 1] << 8 | d[o + 2] for o in range(1, len(d), 3)] if kind in ('add', 'remove') else []
        if kind == 'clear':
            self.subs.clear()
        elif kind == 'add':
            if self.t0 is None:
                self.t0 = now
                print('clock starts: first id list', flush=True)
            for i in ids:
                if i in self.by_id and i not in self.subs:
                    self.due[i] = now
                self.subs.add(i)
        elif kind == 'remove':
            self.subs.difference_update(ids)
        if not self.quiet and kind in ('add', 'remove') and len(ids) < 10:
            print(f'  {kind} {" ".join(f"{i:03X}" for i in ids)}', flush=True)
        sock.sendto(ACK, addr)
        self.counts['acks'] += 1

    def stream(self, sock, now):
        if self.client is None or self.t0 is None or self.silent(now):
            return 0
        phase, values = scene(self.t(now) % CYCLE)
        recs = []
        for cid in sorted(self.subs):
            if cid not in self.by_id or self.due.get(cid, now) > now:
                continue
            recs.append(self.frame(cid, values))
            self.sent_ids[cid] = self.sent_ids.get(cid, 0) + 1
            hz = RATES.get(cid, DEFAULT_HZ)
            self.due[cid] = max(self.due.get(cid, now) + 1 / hz, now - 0.05)
        for o in range(0, len(recs), 64):   # several records a datagram, as the Commander packs them
            sock.sendto(b''.join(recs[o:o + 64]), self.client)
            self.counts['datagrams'] += 1
        self.counts['frames'] += len(recs)
        return len(recs)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--port', type=int, default=1338)
    ap.add_argument('--duration', type=float, default=0, help='seconds, 0 = until Ctrl-C')
    ap.add_argument('--once', action='store_true', help='stop after one 60 s cycle (from the first id list)')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    p = Panda(load_signals(), a.quiet)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((a.bind, a.port))
    sock.setblocking(False)
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    print(f'fake panda on {a.bind}:{a.port}, {len(p.sigs)} signals on {len(p.by_id)} ids', flush=True)
    start = time.monotonic()
    next_tick = next_line = start
    frames_s = 0
    while not stop:
        now = time.monotonic()
        if a.duration and now - start >= a.duration:
            break
        if a.once and p.t0 is not None and now - p.t0 >= CYCLE:
            break
        r, _, _ = select.select([sock], [], [], max(0.0, next_tick - now))
        now = time.monotonic()
        if r:
            while True:
                try:
                    d, addr = sock.recvfrom(2048)
                except BlockingIOError:
                    break
                p.on_datagram(sock, d, addr, now)
        if now >= next_tick:
            next_tick += 0.01
            if next_tick < now:
                next_tick = now + 0.01
            frames_s += p.stream(sock, now)
        if now >= next_line:
            next_line += 1
            t = p.t(now)
            if t is not None:
                phase, v = scene(t % CYCLE)
                if p.silent(now):
                    phase = 'SILENT (link loss)'
                print(f't={t:5.1f} {phase:18} speed {v["speed"]:5.1f} km/h gear {" PRND"[int(v["gear"])]} '
                      f'rear {v["rearKw"]:4.0f} kW soc {v["soc"]:4.1f} | {len(p.subs):2} ids subscribed, '
                      f'{frames_s:4} frames/s', flush=True)
            frames_s = 0
    print(json.dumps({'summary': dict(p.counts, clients=len(p.clients), ids_streamed=len(p.sent_ids),
                                      illegal_hex=p.illegal[:5])}), flush=True)


if __name__ == '__main__':
    main()
