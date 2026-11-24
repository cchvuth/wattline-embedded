#!/usr/bin/env python3
"""mcud against a pty that plays the MCU (no device needed): the hold at start, ctl lines and AT# commands, the AA and
data channels opened by SW/SV and closed by IA, 240-byte paced frames towards the phone, clients that stop reading
dropped while the others are served, the release on SIGUSR1, that a phone's address never reaches the log, and the
MCU's Bluetooth: on (AT#CA) with a session on or starting, off (AT#CB) without one, a client's AT#CB always refused.
  cc -O2 -Wall -o /tmp/mcud mcud.c && python3 -I test_mcud.py /tmp/mcud
"""
import os, pty, select, signal, socket, subprocess, sys, tempfile, time

def frame(cmd, payload=b'', who=0x81):
    body = bytes([who, cmd, len(payload)]) + payload
    x = 0
    for b in body:
        x ^= b
    return b'\x02' + body + bytes([x])

HOLD = bytes.fromhex('0280b005ce01000000fa')
RELEASE = bytes.fromhex('0280b005ce00000000fb')

def frames(buf):
    out = []
    while True:
        i = buf.find(b'\x02')
        if i < 0 or len(buf) - i < 5 or len(buf) - i < buf[i + 3] + 5:
            return out, buf[i:] if i >= 0 else b''
        n = buf[i + 3] + 5
        out.append(buf[i:i + n])
        buf = buf[i + n:]

class Mcu:
    def __init__(self, fd):
        self.fd, self.buf, self.got = fd, b'', []
    def pump(self, t=0.2):
        end = time.time() + t
        while time.time() < end:
            if select.select([self.fd], [], [], 0.02)[0]:
                self.buf += os.read(self.fd, 4096)
                f, self.buf = frames(self.buf)
                self.got += [(time.time(), x) for x in f]
    def send(self, b):
        os.write(self.fd, b)

def flood(mcu, frames, good, each, t=20):
    """the MCU sends frames, 16 at a time, while good reads (`each` bytes a frame); what good got. A batch goes once the
    one before has reached good, so good is never behind; a stalled mcud runs out the deadline instead of hanging"""
    os.set_blocking(mcu.fd, False)
    good.setblocking(False)
    got, out, sent, end = b'', b'', 0, time.time() + t
    while len(got) < each * len(frames) and time.time() < end:
        if not out and sent < len(frames) and len(got) >= each * sent:
            out, sent = b''.join(frames[sent:sent + 16]), min(sent + 16, len(frames))
        r, w, _ = select.select([good], [mcu.fd] if out else [], [], 0.1)
        if w:
            try:
                out = out[os.write(mcu.fd, out):]
            except BlockingIOError:
                pass
        if r:
            c = good.recv(1 << 16)
            if not c:
                break
            got += c
    os.set_blocking(mcu.fd, True)
    good.setblocking(True)
    return got

def recv_until(s, want, t=2.0):
    s.settimeout(t)
    b = b''
    while not b.endswith(want):
        c = s.recv(4096)
        if not c:
            break
        b += c
    return b

def main(binary):
    m, sl = pty.openpty()
    path = os.ttyname(sl)
    d = tempfile.mkdtemp()
    sock = os.path.join(d, 'mcud.sock')
    log = open(os.path.join(d, 'log'), 'w+')
    run_d, data_d = os.path.join(d, 'run'), os.path.join(d, 'data')
    os.mkdir(run_d); os.mkdir(data_d)
    env = dict(os.environ, MCUD_RUN=run_d, MCUD_DATA=data_d, MCUD_AA_GRACE_MS='300', MCUD_AA_KEPT_MS='1500')
    def mark(name, on):
        if on:
            open(os.path.join(run_d, name), 'w').close()
        else:
            os.unlink(os.path.join(run_d, name))
    def with_mcud(test, *args):
        p = subprocess.Popen([binary, path, sock], stdout=subprocess.DEVNULL, stderr=log, env=env)
        try:
            test(p, m, sock, *args)
        finally:
            if p.poll() is None:
                p.kill()
    open(os.path.join(run_d, 'dash-aa'), 'w').close()   # a session on: Bluetooth on, no AT#CB of mcud's own in run()
    with_mcud(run, log)
    os.unlink(os.path.join(run_d, 'dash-aa'))
    with_mcud(bt, run_d)
    with open(os.path.join(data_d, 'aa-on'), 'w') as f:
        f.write('extra\n')
    with_mcud(bt_kept, mark)

def run(p, m, sock, log):
    mcu = Mcu(m)
    for _ in range(50):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    mcu.pump(0.3)
    assert mcu.got and mcu.got[0][1] == HOLD, mcu.got
    assert CA in [x for _, x in mcu.got], 'started with a session on (its mark): Bluetooth not turned on'

    ctl = socket.socket(socket.AF_UNIX); ctl.connect(sock); ctl.sendall(b'ctl\nAT#MZ\n')
    aa = socket.socket(socket.AF_UNIX); aa.connect(sock); aa.sendall(b'aa\n')
    mcu.pump(0.3)
    assert frame(0xD4, b'AT#MZ\r\n', 0x80) in [x for _, x in mcu.got], mcu.got
    ctl.sendall(b'AT#CB\nAT#cb\nAT#MMx\rAT#CB\n'); mcu.pump(0.3)   # refused, whoever asks, however it is spelled
    assert not any(b'AT#CB' in x.upper() for _, x in mcu.got), 'AT#CB reached the MCU'
    aa.setblocking(False)
    try:
        assert aa.recv(10) == b'', 'open before SW'
    except BlockingIOError:
        pass
    aa.setblocking(True)

    mcu.send(frame(0xD4, b'AD0123456789ab\r\n') + frame(0xD4, b'SW\r') + frame(0xD4, b'\n'))   # a line split over frames
    assert recv_until(aa, b'open\n') == b'open\n'
    assert recv_until(ctl, b'SW\n').endswith(b'AD0123456789ab\nSW\n')

    mcu.send(frame(0xD6, bytes([0, 6, 0, 1]) + b'\x11\x13\r\n'))   # raw bytes: XON/XOFF and CR LF untouched
    assert recv_until(aa, b'\r\n') == bytes([0, 6, 0, 1]) + b'\x11\x13\r\n'

    mcu.got.clear()
    aa.sendall(bytes(i & 0xFF for i in range(500)))
    mcu.pump(0.5)
    data = [(t, x) for t, x in mcu.got if x[2] == 0xD6]
    assert [x[3] for _, x in data] == [240, 240, 20], [x[3] for _, x in data]
    assert b''.join(x[4:-1] for _, x in data) == bytes(i & 0xFF for i in range(500))
    assert data[1][0] - data[0][0] >= 0.008 and data[2][0] - data[1][0] >= 0.008, 'not paced'

    mcu.send(frame(0xD4, b'IA\r\n'))
    aa.settimeout(2)
    assert aa.recv(10) == b'', 'aa client not closed on IA'

    ip = socket.socket(socket.AF_UNIX); ip.connect(sock); ip.sendall(b'sv\n')
    mcu.send(frame(0xD4, b'SV\r\n'))
    assert recv_until(ip, b'open\n') == b'open\n'
    aa2 = socket.socket(socket.AF_UNIX); aa2.connect(sock); aa2.sendall(b'aa\n')
    mcu.send(frame(0xD4, b'SW\r\n'))
    assert recv_until(aa2, b'open\n') == b'open\n'
    ip.settimeout(2)
    assert ip.recv(10) == b'', 'sv client not closed when AA took the channel'
    # the same channel announced again: at once, the MCU repeating itself (the client stays); later, the phone having
    # reopened it (the client holding the old opening is closed, a new one gets "open")
    mcu.send(frame(0xD4, b'SW\r\n')); mcu.pump(0.3)
    aa2.setblocking(False)
    try:
        assert aa2.recv(10) != b'', 'aa client closed by a repeated SW'
    except BlockingIOError:
        pass
    aa2.setblocking(True)
    time.sleep(2.1)
    mcu.send(frame(0xD4, b'SW\r\n'))
    aa2.settimeout(2)
    assert aa2.recv(10) == b'', 'aa client not closed when the phone reopened the channel'
    aa2 = socket.socket(socket.AF_UNIX); aa2.connect(sock); aa2.sendall(b'aa\n')
    assert recv_until(aa2, b'open\n') == b'open\n'

    # a client that stops reading is dropped, never waited for (a blocking send to it stalled the one loop that reads
    # the MCU, serves the other clients and sends the hold): the MCU's lines and the phone's bytes still reach the
    # clients that read (the first ctl client has not read since SW either)
    good = socket.socket(socket.AF_UNIX); good.connect(sock); good.sendall(b'ctl\n')
    stuck = socket.socket(socket.AF_UNIX); stuck.connect(sock); stuck.sendall(b'ctl\n')
    stuck_aa = socket.socket(socket.AF_UNIX); stuck_aa.connect(sock); stuck_aa.sendall(b'aa\n')
    mcu.pump(0.3)
    lines = [b'PB%04d' % i + b'x' * 200 for i in range(2000)]   # PB: logged as a length only
    got = flood(mcu, [frame(0xD4, l + b'\r\n') for l in lines], good, 207)
    assert got == b''.join(l + b'\n' for l in lines), 'stalled by a ctl client that stopped reading: %d bytes' % len(got)
    data = bytes(range(240))
    got = flood(mcu, [frame(0xD6, data)] * 2000, aa2, 240)
    assert got == data * 2000, 'stalled by a data client that stopped reading: %d bytes' % len(got)
    for s in (stuck, stuck_aa):   # dropped: what it was sent before, then EOF
        s.settimeout(2)
        while s.recv(1 << 16):
            pass

    mcu.send(b'\x02\x81\xd4\x04SW\r\n\x00')   # a bad XOR: dropped, nothing breaks
    mcu.got.clear()
    p.send_signal(signal.SIGUSR1)
    end = time.time() + 3
    while p.poll() is None and time.time() < end:   # keep reading: tcdrain waits for the pty's reader
        mcu.pump(0.1)
    mcu.pump(0.2)
    assert p.poll() is not None, 'mcud did not exit on SIGUSR1'
    assert RELEASE in [x for _, x in mcu.got], mcu.got

    log.seek(0)
    text = log.read()
    assert '0123456789ab' not in text and 'bt AD (12 bytes)' in text and 'data channel aa open' in text, text
    print('mcud: ok')

CA = frame(0xD4, b'AT#CA\r\n', 0x80)
CB = frame(0xD4, b'AT#CB\r\n', 0x80)

def ready(mcu, sock):
    for _ in range(50):
        if os.path.exists(sock):
            break
        time.sleep(0.05)

def bt(p, m, sock, run_d):
    """No session at the start: Bluetooth off at once; on with a session (dash-aa's marks), off the grace after."""
    mcu = Mcu(m)
    ready(mcu, sock)
    def sent(t):
        mcu.got.clear(); mcu.pump(t)
        return [x for _, x in mcu.got if x in (CA, CB)]
    def touch(*names):
        for n in names:
            open(os.path.join(run_d, n), 'w').close()
    def rm(*names):
        for n in names:
            os.unlink(os.path.join(run_d, n))
    mcu.pump(0.25)   # (inside the grace: off at the start does not wait for it)
    got = [x for _, x in mcu.got]
    assert got.count(CB) == 1 and CA not in got, 'no session at the start: Bluetooth not turned off at once'
    assert sent(1.0) == [], 'AT#CB sent again'
    mcu.send(frame(0xD4, b'IB\r\n'))        # a phone gets in all the same (it was dialling in as Bluetooth went off)
    assert sent(0.4) == [CB], 'a phone connected with Bluetooth off: not turned off again'
    assert not os.path.exists(os.path.join(run_d, 'mcud.phone')), 'a phone marked connected with Bluetooth off'
    mcu.send(frame(0xD4, b'IA\r\n')); mcu.pump(0.1)
    touch('dash-aa.starting')                 # the AA button: on at once
    assert sent(0.8) == [CA], 'a session starting: Bluetooth not on'
    rm('dash-aa.starting'); assert sent(1.5) == [CB], 'the start gone: Bluetooth not off'
    touch('dash-aa.starting'); mcu.send(frame(0xD4, b'IB\r\n'))   # a phone in the same moment as the button: on, kept
    assert sent(0.8) == [CA], 'a phone connecting as a session starts: turned off again (or not on)'
    assert os.path.exists(os.path.join(run_d, 'mcud.phone')), 'a phone connecting as a session starts: not marked'
    mcu.send(frame(0xD4, b'IA\r\n')); mcu.pump(0.1)
    touch('dash-aa'); rm('dash-aa.starting')  # up
    assert sent(0.8) == [], 'Bluetooth told again while the session is on'
    rm('dash-aa')                             # stopped: off, the grace after
    assert sent(0.15) == [], 'Bluetooth off before the grace'
    assert sent(1.5) == [CB], 'the session stopped: Bluetooth not off'
    touch('dash-aa', 'dash-aa.extra')         # the other mode: on as well
    assert sent(0.8) == [CA], 'extra mode: Bluetooth not on'
    rm('dash-aa'); touch('dash-aa.starting')  # a switch to the other mode inside the grace: stays on
    assert sent(0.2) == [] , 'Bluetooth told during a switch'
    touch('dash-aa'); rm('dash-aa.starting')
    assert sent(0.8) == [], 'Bluetooth off during a switch of modes'
    ctl = socket.socket(socket.AF_UNIX); ctl.connect(sock); ctl.sendall(b'ctl\nAT#CB\nAT#cb\n')
    assert sent(0.5) == [], "a client's AT#CB reached the MCU"
    ctl.close()
    rm('dash-aa', 'dash-aa.extra')
    assert sent(1.5) == [CB], 'extra stopped: Bluetooth not off'
    cs = []
    for kind in (b'aa\n', b'sv\n', b'ctl\n'):   # clients alone are no session (a helper may outlive its own): still off
        a = socket.socket(socket.AF_UNIX); a.connect(sock); a.sendall(kind); cs.append(a)
    assert sent(0.8) == [], 'a client with no session on turned Bluetooth on'
    for a in cs:
        a.close()
    touch('dash-aa'); mcu.pump(0.8)
    mcu.send(frame(0xD4, b'IB\r\n')); mcu.pump(0.2)   # a phone connected: marked for dash-aa (no call needed)
    assert os.path.exists(os.path.join(run_d, 'mcud.phone')), 'IB: no mcud.phone'
    mcu.send(frame(0xD4, b'IA\r\n')); mcu.pump(0.2)
    assert not os.path.exists(os.path.join(run_d, 'mcud.phone')), 'IA: mcud.phone left'
    mcu.send(frame(0xD4, b'IB\r\n')); mcu.pump(0.2)   # connected as the session ends, and the MCU says no IA: unmarked
    rm('dash-aa')
    assert sent(1.5) == [CB], 'the session stopped with a phone on: Bluetooth not off'
    assert not os.path.exists(os.path.join(run_d, 'mcud.phone')), 'Bluetooth off: mcud.phone left (the next start would not call)'
    p.terminate(); p.wait(3)
    print('mcud Bluetooth on and off: ok')

def bt_kept(p, m, sock, mark):
    """A session kept on (aa-on, either mode) at the start: Bluetooth on for the dash's restore, on through it once it
    starts (its lock, then its mark, past the window); stopped, or one that never came: off."""
    mcu = Mcu(m)
    ready(mcu, sock)
    mcu.pump(0.6)
    got = [x for _, x in mcu.got if x in (CA, CB)]
    assert got == [CA], 'a session kept on at the start: Bluetooth not on (or turned off)'
    mark('dash-aa.starting', True)   # the restore, inside the window (1.5 s here)
    mcu.got.clear(); mcu.pump(2.5)   # ... still starting past it
    assert [x for _, x in mcu.got if x in (CA, CB)] == [], 'Bluetooth told during the restore (off past the window?)'
    mark('dash-aa.starting', False)  # the restore failed (its lock gone, no mark; aa-on left): off, the window being over
    mcu.got.clear(); mcu.pump(1.5)
    assert [x for _, x in mcu.got if x in (CA, CB)] == [CB], 'the restore failed: Bluetooth not off after its window'
    p.terminate(); p.wait(3)
    print('mcud Bluetooth, a session kept on: ok')

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '/tmp/mcud')
