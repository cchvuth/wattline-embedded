#!/usr/bin/env python3 -I
"""Host test for bt-fwcutter: python3 -I package/bt-fwcutter/test.py  (needs cc)

1. Synthetic: a binary with a decoy string, v1 patches for other projects, a v2 patch, the wanted v1 patch and config
   blocks (one with a BD-address entry) must give exactly the wanted patch and a UART-only config; two different
   patches for the same project must be refused.
2. Optional, against a real Bluetooth daemon binary and the files cut from it some other way: set BT_FWCUTTER_BINARY,
   BT_FWCUTTER_REF_FW, BT_FWCUTTER_REF_CFG. Skipped when unset or missing. Those files are never part of this tree."""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXT = b'\x51\x04\xfd\x77'
MAGIC = b'\x55\xab\x23\x87'

def v1(pid, chips=(1, 3), pad=17):
    n = len(chips)
    h = 14 + 8 * n
    bodies = [bytes([0xa0 + k]) * (40 + k) for k in range(n)]
    offs, o = [], h
    for b in bodies:
        offs.append(o); o += len(b)
    return (b'Realtech' + struct.pack('<IH', 0x1234 + pid, n) + struct.pack('<%dH' % n, *chips)
            + struct.pack('<%dH' % n, *map(len, bodies)) + struct.pack('<%dI' % n, *offs) + b''.join(bodies)
            + b'\0' * pad + bytes([0xff, pid, 1, 0]) + EXT)

def v2(pid):
    secs = struct.pack('<II', 0xa, 5) + b'12345' + struct.pack('<II', 0xb, 3) + b'abc'
    return b'RTBTCore' + b'\x01' * 8 + struct.pack('<I', 2) + secs + bytes([pid, 1, 0]) + EXT

def cfg(entries):
    body = b''.join(struct.pack('<HB', o, len(v)) + v for o, v in entries)
    return MAGIC + struct.pack('<H', len(body)) + body

UART = bytes.fromhex('028092040000000000000000a0000000')

def run(exe, blob, *args):
    with tempfile.TemporaryDirectory() as t:
        b = os.path.join(t, 'bin'); open(b, 'wb').write(blob)
        fw, cf = os.path.join(t, 'fw'), os.path.join(t, 'cfg')
        r = subprocess.run([exe, *args, b, fw, cf], capture_output=True, text=True)
        out = (open(fw, 'rb').read(), open(cf, 'rb').read()) if r.returncode == 0 else None
        return r.returncode, out, r.stderr

def main():
    t = tempfile.mkdtemp()
    exe = os.path.join(t, 'bt-fwcutter')
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-o', exe,
                    os.path.join(HERE, 'src', 'bt-fwcutter.c')], check=True)

    want = v1(19)
    blob = (b'\x7fELF junk RealtechRealtech ' + v1(14) + b'xx' + cfg([(0x0c, UART), (0x30, b'\x11' * 6)])
            + b'Realtech\x00\x00' + v2(20) + want + MAGIC + b'\xff\xff' + v1(10) + cfg([(0x0c, UART)]) + b'tail')
    rc, out, err = run(exe, blob)
    assert rc == 0, err
    assert out[0] == want, 'wrong patch'
    assert out[1] == cfg([(0x0c, UART)]), 'config must hold the UART entry only'
    assert 'RTBTCore' in err and 'project id 20' in err, err

    rc, out, err = run(exe, blob, '-p', '20')
    assert rc == 0 and out[0] == v2(20), err

    rc, _, err = run(exe, blob + v1(19, pad=3))
    assert rc == 2 and 'refusing' in err, err
    rc, _, _ = run(exe, blob + cfg([(0x0c, UART[:4] + b'\0' * 12)]))
    assert rc == 2
    rc, _, _ = run(exe, b'nothing here')
    assert rc == 2
    print('synthetic: ok')

    paths = [os.environ.get(k, '') for k in ('BT_FWCUTTER_BINARY', 'BT_FWCUTTER_REF_FW', 'BT_FWCUTTER_REF_CFG')]
    if not all(p and os.path.isfile(p) for p in paths):
        print('reference: skipped (BT_FWCUTTER_BINARY / _REF_FW / _REF_CFG not set or missing)')
        return
    rc, out, err = run(exe, open(paths[0], 'rb').read())
    sys.stderr.write(err)
    assert rc == 0
    assert out[0] == open(paths[1], 'rb').read(), 'firmware differs from the reference'
    ref = open(paths[2], 'rb').read()
    assert out[1] == ref, 'config differs from the reference'
    baud, = struct.unpack_from('<I', out[1], 9)
    print('reference: firmware and config byte-identical (%d bytes; UART word 0x%08x, flow control %s)'
          % (len(out[0]), baud, bool(out[1][9 + 12] & 4)))

if __name__ == '__main__':
    main()
