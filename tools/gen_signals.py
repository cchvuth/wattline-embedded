#!/usr/bin/env python3
"""kSignals (lib/signals.dart) + tools/extra_signals.json -> C table + decoder, value tables, the
subscribe list and lib/panda.dart's polling schedule; obd_test.dart decode() vectors -> C test.
Usage: python3 -I gen_signals.py <repo> <outdir>   (reads the repo, writes only <outdir>)"""
import re, sys, os, json

repo, out = sys.argv[1], sys.argv[2]
src = open(os.path.join(repo, 'lib/signals.dart')).read()
table = src[src.index('const kSignals'):src.index('\n};', src.index('const kSignals'))]

def sig_fields(args):
    a = [x.strip() for x in args.split(',') if x.strip()]
    f = dict(id=int(a[0], 0), start=int(a[1]), len=int(a[2]), scale='1', offset='0',
             signed=0, sna=-1, mux=-1, muxStart=0, muxLen=3)
    for kv in a[3:]:
        k, v = [s.strip() for s in kv.split(':')]
        f[k] = {'signed': lambda v: int(v == 'true')}.get(k, lambda v: v)(v)
    return f

sigs = [(n, sig_fields(a)) for n, a in re.findall(r"'(\w+)':\s*Sig\(([^)]*)\)", table)]
assert len(sigs) == table.count('Sig('), (len(sigs), table.count('Sig('))  # every row parsed
phone = len(sigs)
phone_ids = list(dict.fromkeys(f['id'] for _, f in sigs))   # kSubscribeIds: first-seen order

# ---- the Dash's own rows (0x318 for the clock), after the phone's: their order and ids stay ----
ex = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'extra_signals.json')))
for n, e in ex['signals'].items():
    assert n not in dict(sigs), n
    f = dict(id=int(e['id'], 0), start=e['start'], len=e['len'], scale=str(e.get('scale', 1)),
             offset=str(e.get('offset', 0)), signed=int(e.get('signed', False)), sna=e.get('sna', -1),
             mux=e.get('mux', -1), muxStart=e.get('muxStart', 0), muxLen=e.get('muxLen', 3))
    sigs.append((n, f))
sub_ids = list(dict.fromkeys(f['id'] for _, f in sigs))
assert sub_ids[:len(phone_ids)] == phone_ids

row = lambda n, f: (f'  {{"{n}", 0x{f["id"]:03X}, {f["start"]}, {f["len"]}, {f["scale"]}, {f["offset"]}, '
                    f'{f["signed"]}, {f["sna"]}, {f["mux"]}, {f["muxStart"]}, {f["muxLen"]}}},')
hdr = f'''// Generated from lib/signals.dart (+ tools/extra_signals.json) by gen_signals.py: {len(sigs)} signals, kSignals order.
#include <stdint.h>
typedef struct {{ const char *name; uint16_t id; uint8_t start, len; double scale, offset;
  uint8_t is_signed; int32_t sna; int16_t mux; uint8_t mux_start, mux_len; }} sig_t; // sna/mux -1 = none
static const sig_t kSignals[] = {{
{chr(10).join(row(n, f) for n, f in sigs)}
}};
enum {{ kSignalCount = {len(sigs)}, kPhoneSignalCount = {phone} }};
enum {{ {", ".join("SIG_" + n for n, _ in sigs)} }};
// Intel (DBC @1) bit order, as signals.dart _bits().
static inline uint64_t sig_bits(const uint8_t *d, int start, int len) {{
  uint64_t v = 0;
  for (int i = 0; i < len; i++) {{ int b = start + i; if ((d[b >> 3] >> (b & 7)) & 1) v |= 1ull << i; }}
  return v;
}}
// signals.dart decode(): 0 = null (too short, other mux page, SNA).
static inline int sig_decode(const sig_t *s, const uint8_t *d, int n, double *out) {{
  if ((s->start + s->len + 7) / 8 > n) return 0;
  if (s->mux >= 0 && (int64_t)sig_bits(d, s->mux_start, s->mux_len) != s->mux) return 0;
  int64_t v = (int64_t)sig_bits(d, s->start, s->len);
  if (s->sna >= 0 && v == s->sna) return 0;          // raw unsigned compare, before the sign
  if (s->is_signed && (v & (1ll << (s->len - 1)))) v -= 1ll << s->len;
  *out = (double)v * s->scale + s->offset;           // double: tests use 1e-9 tolerances
  return 1;
}}
'''

# ---- value tables (signals.dart) ----
def dart_map(name):
    m = re.search(r'const ' + name + r' = \{([^}]*)\}', src)
    return {int(k): v for k, v in re.findall(r"(\d+):\s*'([^']*)'", m[1])}
def lut(name):
    d = dart_map(name)
    cells = ', '.join(f'"{d[k]}"' if k in d else '0' for k in range(max(d) + 1))
    return f'static const char *const {name}[{max(d) + 1}] = {{{cells}}};'
latch = [int(x) for x in re.search(r'const kLatchOpen = \{([^}]*)\}', src)[1].split(',')]
hdr += '\n// Value tables: index by the raw value; LUT() is NULL for a value without a name (a Dart null).\n'
hdr += '\n'.join(lut(n) for n in ['kGears', 'kAutopilot', 'kChargeWhy', 'kRecirc', 'kDome', 'kFan2', 'kAutoBlower'])
hdr += '\n#define LUT(t, k) ((k) >= 0 && (k) < (int)(sizeof t / sizeof *t) ? t[k] : (const char *)0)\n'
hdr += f'static inline int latch_open(int v) {{ return {" || ".join(f"v == {x}" for x in latch)}; }}\n'

# ---- kSubscribeIds, and which signals each id carries (kSignals order: the derivations rely on it) ----
hdr += f'\nenum {{ kSubscribeCount = {len(sub_ids)}, kPhoneSubscribeCount = {len(phone_ids)} }};\n'
hdr += f'static const uint16_t kSubscribeIds[] = {{{", ".join(f"0x{i:03X}" for i in sub_ids)}}};\n'
first, flat = [], []
for i in sub_ids:
    first.append(len(flat)); flat += [k for k, (_, f) in enumerate(sigs) if f['id'] == i]
first.append(len(flat))
hdr += f'static const uint8_t kIdSigs[] = {{{", ".join(map(str, flat))}}};\n'
hdr += f'static const uint8_t kIdFirst[] = {{{", ".join(map(str, first))}}}; // kIdSigs[kIdFirst[n] .. kIdFirst[n+1]) for kSubscribeIds[n]\n'
hdr += 'static inline int sub_index(unsigned id) { for (int n = 0; n < kSubscribeCount; n++) if (kSubscribeIds[n] == id) return n; return -1; }\n'

# ---- lib/panda.dart's polling schedule and Bluetooth field list ----
pd = open(os.path.join(repo, 'lib/panda.dart')).read()
nocomment = lambda b: re.sub(r'//[^\n]*', '', b)
always = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', nocomment(re.search(r'alwaysOn = <int>\{(.*?)\};', pd, re.S)[1]))]
always += [int(x, 0) for x in ex['alwaysOn']]
sparse = [(p == 'true', [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', ids)]) for p, ids in
          re.findall(r'\(parked: (true|false), ids: \[([^\]]*)\]\)', nocomment(re.search(r'_sparse = <.*?>\[(.*?)\n  \];', pd, re.S)[1]))]
wheel = [[int(x) for x in g.split(',') if x.strip()] for g in
         re.findall(r'\[([\d, ]*)\]', nocomment(re.search(r'_wheel = <List<int>>\[(.*?)\n  \];', pd, re.S)[1]))]
groups = re.search(r'_chargeGroup = (\d+), _steerGroup = (\d+)', pd)
chips = [int(x) for x in re.search(r'_chipGroups = \{([^}]*)\}', pd)[1].split(',')]
btf = [int(x) for x in re.search(r'static const btFields = \[([^\]]*)\]', pd)[1].replace('\n', ' ').split(',') if x.strip()]
assert sparse and wheel and len(always) == len(set(always)) and all(len(ids) <= 8 for _, ids in sparse)
hdr += f'\n// Panda.alwaysOn (+ the Dash\'s), _sparse, _wheel, _chargeGroup/_steerGroup/_chipGroups, btFields.\n'
hdr += f'enum {{ kAlwaysOnCount = {len(always)}, kGroupCount = {len(sparse)}, kWheelSlots = {len(wheel)}, '
hdr += f'kChargeGroup = {groups[1]}, kSteerGroup = {groups[2]}, kBtFieldCount = {len(btf)} }};\n'
hdr += f'static const uint16_t kAlwaysOn[] = {{{", ".join(f"0x{i:03X}" for i in always)}}};\n'
hdr += 'typedef struct { uint8_t parked, n; uint16_t ids[8]; } group_t;\n'
hdr += 'static const group_t kSparse[] = {\n' + '\n'.join(
    f'  {{{int(pk)}, {len(ids)}, {{{", ".join(f"0x{i:03X}" for i in ids)}}}}},' for pk, ids in sparse) + '\n};\n'
hdr += f'static const uint8_t kWheel[][3] = {{{", ".join("{" + ", ".join(map(str, [len(g)] + g)) + "}" for g in wheel)}}}; // count, groups\n'
assert all(len(g) <= 2 for g in wheel)
hdr += f'static const uint8_t kChipGroups[] = {{{", ".join(map(str, chips))}}};\n'
hdr += f'static const uint8_t kBtFields[] = {{{", ".join(map(str, btf))}}};\n'
open(os.path.join(out, 'signals_gen.h'), 'w').write(hdr)

# ---- vectors from test/obd_test.dart: decode(...) expectations only (stateless) ----
t = open(os.path.join(repo, 'test/obd_test.dart')).read()
def _int(x):   # a literal, or literals joined by | (obd_test.dart writes 0x38 | 0x01)
    v = 0
    for part in x.split('|'): v |= int(part, 0)
    return v
byts = lambda s: [_int(x) for x in s.split(',') if x.strip()]
vars_ = {m[0]: byts(m[1]) for m in re.findall(r'final (\w+) = \[([^\]]*)\];', t)}
maps = {}
for name in ['kGears', 'kDome', 'kRecirc', 'kAutopilot']:
    m = re.search(name + r' = \{([^}]*)\}', src)
    maps[name] = {int(k): v for k, v in re.findall(r"(\d+):\s*'([^']*)'", m[1])}
names = [n for n, _ in sigs]
cases = []
pat = re.compile(r"expect\((?:(k\w+)\[)?decode\((?:kSignals\['(\w+)'\]!|const Sig\(([^)]*)\)),\s*"
                 r"(\[[^\]]*\]|\w+)\)!?(?:\.round\(\)\])?,\s*(.+?)\);(?:\s*//.*)?$", re.M)
for lut, sname, inline, arg, exp in pat.findall(t):
    data = byts(arg[1:-1]) if arg.startswith('[') else vars_[arg]
    if inline:
        f = sig_fields(inline)
        sig = (f'(sig_t){{"inline", {f["id"]}, {f["start"]}, {f["len"]}, {f["scale"]}, {f["offset"]}, '
               f'{f["signed"]}, {f["sna"]}, {f["mux"]}, {f["muxStart"]}, {f["muxLen"]}}}')
    else:
        sig = f'kSignals[{names.index(sname)}]'
    if exp == 'isNull' and not lut:
        kind, a, b = 0, 0, 0
    elif lut:   # map lookup: expected key, or "not a key" for isNull
        keys = maps[lut]
        if exp == 'isNull':
            kind, a, b = 3, 0, 0
            sig += f'/*{lut}*/'
        else:
            kind, a, b = 1, [k for k, v in keys.items() if v == exp.strip("'")][0], 0
    elif exp.startswith('closeTo'):
        x, tol = [v.strip() for v in exp[8:-1].split(',')]
        kind, a, b = 2, x, tol
    else:
        kind, a, b = 1, exp, 0
    notkeys = '{' + ','.join(map(str, maps[lut])) + '}' if lut and kind == 3 else '{0}'
    nk = len(maps[lut]) if lut and kind == 3 else 0
    cases.append(f'  {{ &{sig}, {{{",".join(map(str, data)) or "0"}}}, {len(data)}, {kind}, {a}, {b}, {notkeys}, {nk} }},'
                 .replace('&(sig_t)', '&(sig_t)'))

app_cases = len(cases)
for v in ex['vectors']:   # the Dash's own rows
    d = v['data']
    cases.append(f'  {{ &kSignals[{names.index(v["sig"])}], {{{",".join(map(str, d))}}}, {len(d)}, 1, {v["expect"]}, 0, {{0}}, 0 }},')
open(os.path.join(out, 'test_decode.c'), 'w').write(f'''// {len(cases)} decode() vectors from test/obd_test.dart. kind 0 null, 1 == a, 2 |v-a|<=b, 3 not a map key.
#include <stdio.h>
#include <math.h>
#include "signals_gen.h"
typedef struct {{ const sig_t *s; uint8_t d[8]; int n, kind; double a, b; int keys[16]; int nkeys; }} vec_t;
int main(void) {{
  vec_t v[] = {{
{chr(10).join(cases)}
  }};
  int fail = 0, n = sizeof v / sizeof v[0];
  for (int i = 0; i < n; i++) {{
    double x = 0; int ok = sig_decode(v[i].s, v[i].d, v[i].n, &x), pass;
    if (v[i].kind == 0) pass = !ok;
    else if (v[i].kind == 1) pass = ok && x == v[i].a;
    else if (v[i].kind == 2) pass = ok && fabs(x - v[i].a) <= v[i].b;
    else {{ pass = ok; for (int k = 0; k < v[i].nkeys; k++) if (lround(x) == v[i].keys[k]) pass = 0; }}
    if (!pass) {{ fail++; printf("FAIL #%d %s ok=%d x=%g\\n", i, v[i].s->name, ok, x); }}
  }}
  printf("%d/%d decode vectors pass\\n", n - fail, n);
  return fail != 0;
}}
''')
print(f'{phone} + {len(sigs) - phone} signals, {len(sub_ids)} ids, {app_cases} + {len(cases) - app_cases} vectors')
