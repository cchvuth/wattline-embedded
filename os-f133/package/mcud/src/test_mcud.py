#!/usr/bin/env python3
"""mcud against a pty that plays the MCU (no device needed): the hold at start, ctl lines and AT# commands, the AA and
data channels opened by SW/SV and closed by IA, 240-byte paced frames towards the phone, clients that stop reading
dropped while the others are served, the release on SIGUSR1, that a phone's address never reaches the log, and that a
phone opening the AA channel while Android Auto is off is disconnected (AT#CD), and never while it is on or starting.
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
    env = dict(os.environ, MCUD_RUN=run_d, MCUD_DATA=data_d, MCUD_AA_GRACE_MS='300', MCUD_AA_KEPT_MS='600000')
    p = subprocess.Popen([binary, path, sock], stdout=subprocess.DEVNULL, stderr=log, env=env)
    try:
        run(p, m, sock, log)
    finally:
        if p.poll() is None:
            p.kill()
    p = subprocess.Popen([binary, path, sock], stdout=subprocess.DEVNULL, stderr=log, env=env)
    try:
        aa_off(p, m, sock, run_d, data_d)
    finally:
        if p.poll() is None:
            p.kill()
    # a phone that reopens its channel every 2.2 s (each a new opening, no IA between) is still dropped once AA has been
    # off for the grace (3 s here): the reopening must not start the count again
    p = subprocess.Popen([binary, path, sock], stdout=subprocess.DEVNULL, stderr=log, env=dict(env, MCUD_AA_GRACE_MS='3000'))
    try:
        mcu = Mcu(m)
        for _ in range(50):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        mcu.pump(0.3); mcu.got.clear()
        for _ in range(4):
            mcu.send(frame(0xD4, b'SW\r\n')); mcu.pump(2.2)
        assert [x for _, x in mcu.got].count(CD) >= 1, 'a phone reopening its channel every 2.2 s was not dropped'   # (no IA here: again 3 s on)
        p.terminate(); p.wait(3)
        print('mcud AA off, reopened channel: ok')
    finally:
        if p.poll() is None:
            p.kill()

def run(p, m, sock, log):
    mcu = Mcu(m)
    for _ in range(50):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    mcu.pump(0.3)
    assert mcu.got and mcu.got[0][1] == HOLD, mcu.got

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

CD = frame(0xD4, b'AT#CD\r\n', 0x80)
MH = frame(0xD4, b'AT#MH\r\n', 0x80)

def aa_off(p, m, sock, run_d, data_d):
    mcu = Mcu(m)
    for _ in range(50):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    mcu.pump(0.3)
    def cds(t):
        mcu.got.clear(); mcu.pump(t)
        return [x for _, x in mcu.got].count(CD)
    def channel(open_):
        mcu.send(frame(0xD4, b'SW\r\n' if open_ else b'IA\r\n')); mcu.pump(0.05)
    def touch(*names):
        for n in names:
            open(os.path.join(run_d, n), 'w').close()
    def rm(*names):
        for n in names:
            os.unlink(os.path.join(run_d, n))
    def reset_cap():   # an AA start (an aa client) clears the cap
        a = socket.socket(socket.AF_UNIX); a.connect(sock); a.sendall(b'aa\n'); mcu.pump(0.1); a.close(); mcu.pump(0.1)

    channel(True)
    assert cds(0.15) == 0, 'dropped before the grace'
    assert cds(0.8) == 1, 'AA off: the phone was not disconnected once'
    got = [x for _, x in mcu.got]
    assert MH in got and got.index(MH) < got.index(CD), 'auto-connect not off (AT#MH) before the drop'
    mcu.send(frame(0xD4, b'IB\r\n')); mcu.pump(0.2)   # a phone connected: marked for dash-aa (no call back needed)
    assert os.path.exists(os.path.join(run_d, 'mcud.phone')), 'IB: no mcud.phone'
    channel(False)
    assert not os.path.exists(os.path.join(run_d, 'mcud.phone')), 'IA: mcud.phone left'

    touch('dash-aa'); channel(True)   # AA on: the phone stays
    assert cds(0.8) == 0, 'disconnected with AA on'
    rm('dash-aa')                     # AA stopped while the channel stays open: dropped after the grace
    assert cds(1.0) == 1, 'not disconnected after AA stopped with the channel open'
    channel(False)

    touch('dash-aa', 'dash-aa.extra'); channel(True)   # the other mode up: no Android Auto behind the channel
    assert cds(1.0) == 1, 'extra mode counted as Android Auto'
    channel(False); rm('dash-aa', 'dash-aa.extra')
    reset_cap()

    touch('dash-aa.starting'); channel(True)
    assert cds(0.8) == 0, 'disconnected while AA was starting'
    channel(False); rm('dash-aa.starting')
    with open(os.path.join(data_d, 'aa-on'), 'w') as f:
        f.write('aa\n')               # kept on, inside mcud's first minutes: the restore's turn
    channel(True)
    assert cds(0.8) == 0, 'disconnected with AA kept on, before its restore'
    channel(False)
    with open(os.path.join(data_d, 'aa-on'), 'w') as f:
        f.write('extra\n')
    channel(True)
    assert cds(1.0) == 1, 'kept-on extra mode counted as Android Auto'
    channel(False); os.unlink(os.path.join(data_d, 'aa-on'))

    a = socket.socket(socket.AF_UNIX); a.connect(sock); a.sendall(b'aa\n'); mcu.pump(0.1)   # a receiver waiting
    channel(True)
    assert cds(0.8) == 0, 'disconnected with an aa client there'
    a.close(); channel(False)
    channel(True); channel(False)
    assert cds(0.8) == 0, 'disconnected after the channel had closed by itself'

    for i in range(3):   # the cap: three drops (the aa client above cleared it), then the phone is left alone
        channel(True); n = cds(1.0); channel(False)
        assert n == 1, 'drop %d not sent' % (i + 1)
    channel(True)
    assert cds(1.2) == 0, 'a fourth drop within 10 minutes'
    channel(False)
    reset_cap(); channel(True)
    assert cds(1.0) == 1, 'an AA start did not clear the cap'
    p.terminate(); p.wait(3)
    print('mcud AA off: ok')

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '/tmp/mcud')
