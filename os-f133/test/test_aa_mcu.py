#!/usr/bin/env python3
"""aa mcu (Android Auto's Bluetooth start through the screen's MCU) end to end, no device: mcud on a pty that plays the
MCU and the phone. The phone opens the channel (SW); aa sends WifiStartRequest {ip, port} in D6 frames, answers the
phone's WifiInfoRequest (split over two frames) with the AP's credentials and a repeated one again, holds the link
until the MCU closes it (IA), then serves the next opening; the passphrase reaches neither log.
Both binaries built for the machine that runs it (aa needs Linux headers), from the repository's root:
  cc -O2 -o /tmp/mcud os-f133/package/mcud/src/mcud.c &&
  cc -O2 -DAA_MCU=1 -o /tmp/aa app/aa/aa.c app/core/pb.c -lssl -lcrypto -lpthread &&
  python3 -I os-f133/test/test_aa_mcu.py /tmp/mcud /tmp/aa
"""
import os, pty, select, struct, subprocess, sys, tempfile, time

IP, SSID, KEY, BSSID, PORT = '192.168.53.1', 'WattlineAA', '0123456789abcdef0123', '02:11:22:33:44:55', 5288

def frame(cmd, payload=b'', who=0x81):
    body = bytes([who, cmd, len(payload)]) + payload
    x = 0
    for b in body:
        x ^= b
    return b'\x02' + body + bytes([x])

def line(s):
    return frame(0xD4, s.encode() + b'\r\n')

def msg(mid, pb=b''):
    return struct.pack('>HH', len(pb), mid) + pb

def pb_fields(b):   # varint and length-delimited fields only (all the handshake uses)
    out, i = {}, 0
    def varint():
        nonlocal i
        v = s = 0
        while True:
            c = b[i]; i += 1
            v |= (c & 0x7F) << s; s += 7
            if c < 0x80:
                return v
    while i < len(b):
        k = varint()
        if k & 7 == 0:
            out[k >> 3] = varint()
        elif k & 7 == 2:
            n = varint(); out[k >> 3] = b[i:i + n]; i += n
        else:
            raise ValueError('wire type %d' % (k & 7))
    return out

class Mcu:   # the pty's far end: the frames mcud writes, the bytes it sends the phone (D6) as one stream
    def __init__(self, fd):
        self.fd, self.buf, self.data = fd, b'', b''
    def pump(self, t):
        end = time.time() + t
        while time.time() < end:
            if select.select([self.fd], [], [], 0.02)[0]:
                self.buf += os.read(self.fd, 4096)
            while True:
                i = self.buf.find(b'\x02')
                if i < 0 or len(self.buf) - i < 5 or len(self.buf) - i < self.buf[i + 3] + 5:
                    break
                n = self.buf[i + 3] + 5
                f, self.buf = self.buf[i:i + n], self.buf[i + n:]
                if f[1] == 0x80 and f[2] == 0xD6:
                    self.data += f[4:-1]
    def message(self, t=4.0):   # the next whole [len][id][protobuf] message aa sent the phone
        end = time.time() + t
        while time.time() < end:
            if len(self.data) >= 4:
                n, mid = struct.unpack('>HH', self.data[:4])
                if len(self.data) >= 4 + n:
                    pb, self.data = self.data[4:4 + n], self.data[4 + n:]
                    return mid, pb_fields(pb)
            self.pump(0.05)
        raise AssertionError('no message from aa (have %r)' % self.data)
    def send(self, b):
        os.write(self.fd, b)

def main(mcud, aa):
    m, sl = pty.openpty()
    d = tempfile.mkdtemp()
    sock = os.path.join(d, 'mcud.sock')
    mlog, alog = open(os.path.join(d, 'mcud.log'), 'w+'), open(os.path.join(d, 'aa.log'), 'w+')
    p = subprocess.Popen([mcud, os.ttyname(sl), sock], stdout=subprocess.DEVNULL, stderr=mlog)
    for _ in range(50):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    a = subprocess.Popen([aa, 'mcu', IP, SSID, '-', BSSID], env=dict(os.environ, MCUD_SOCK=sock),
                         stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=alog)
    a.stdin.write((KEY + '\n').encode()); a.stdin.close()   # the passphrase on stdin, as dash-aa gives it
    time.sleep(0.3)
    assert KEY.encode() not in open('/proc/%d/cmdline' % a.pid, 'rb').read(), 'the passphrase in the command line'
    try:
        run(Mcu(m), mlog, alog)
    finally:
        for x in (a, p):
            if x.poll() is None:
                x.kill()

def run(mcu, mlog, alog):
    mcu.pump(0.5)   # aa connected and waiting
    for round in (1, 2):
        mcu.send(line('SW'))   # the phone opened the Android Auto service
        mid, f = mcu.message()
        assert mid == 1 and f[1] == IP.encode() and f[2] == PORT, (mid, f)   # WifiStartRequest
        req = msg(2)
        mcu.send(frame(0xD6, req[:2]) + frame(0xD6, req[2:]))   # WifiInfoRequest, split over two frames
        mid, f = mcu.message()
        assert mid == 3 and f == {1: SSID.encode(), 2: KEY.encode(), 3: BSSID.encode(), 4: 8, 5: 1}, (mid, f)
        if round == 1:
            mcu.send(frame(0xD6, msg(7, b'\x0a\x0c' + IP.encode() + b'\x10\xa8\x29')) + frame(0xD6, msg(6, b'\x08\x00')))
            mcu.send(frame(0xD6, msg(2)))   # asked again (a phone whose join failed): answered again
            mid, f = mcu.message()
            assert mid == 3 and f[2] == KEY.encode(), (mid, f)
            mcu.send(line('IA'))   # the phone's HFP link gone: mcud closes the channel, aa waits for the next
            mcu.pump(0.5)
    for log in (mlog, alog):
        log.seek(0)
    text = alog.read()
    assert KEY not in text and KEY not in mlog.read(), 'the passphrase in a log'
    assert text.count('handing AP creds') == 2 and text.count('the channel closed') == 1, text
    assert 'BT rx id 7' in text and 'BT rx id 6' in text and 'failed' not in text, text
    print('aa mcu (passphrase on stdin): ok')

if __name__ == '__main__':
    main(*sys.argv[1:3])
