#!/usr/bin/env python3
"""Turns the exported icons and car-lights layers (export_test.dart) and the app's car pictures into
LVGL image descriptors: ui/gen/assets.c and ui/gen/assets.h.
usage: gen.py <export dir> <app assets dir> <out dir>"""
import json
import os
import sys

from PIL import Image

exp, app, out = sys.argv[1:4]
geo = json.load(open(os.path.join(exp, 'geometry.json')))
cab, px = geo['cab'], geo['px']
CAR_H = round(236 * cab * px)  # the cabin's car height on the Dash, px
REGEN, ALERT = (0x3D, 0xDC, 0x97), (0xE2, 0x3D, 0x3D)

c_out, h_out = [], []


def emit(name, img, a8=False):
    w, h = img.size
    if a8:
        data = img.getchannel('A').tobytes()
        cf, stride = 'LV_COLOR_FORMAT_A8', w
    else:
        r, g, b, a = img.convert('RGBA').split()
        data = Image.merge('RGBA', (b, g, r, a)).tobytes()  # lv_color32_t: blue, green, red, alpha
        cf, stride = 'LV_COLOR_FORMAT_ARGB8888', w * 4
    body = ','.join(map(str, data))
    c_out.append(f'static const uint8_t {name}_px[] = {{{body}}};\n'
                 f'const lv_image_dsc_t {name} = {{.header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = {cf}, '
                 f'.w = {w}, .h = {h}, .stride = {stride}}}, .data_size = {len(data)}, .data = {name}_px}};\n')
    h_out.append(f'extern const lv_image_dsc_t {name};\n')


def cropped(img):
    box = img.getchannel('A').point(lambda v: 255 if v > 2 else 0).getbbox()
    return (None, 0, 0) if box is None else (img.crop(box), box[0], box[1])


def modulate(img, rgb):
    r, g, b, a = img.split()
    return Image.merge('RGBA', [ch.point(lambda v, k=k: v * k // 255) for ch, k in zip((r, g, b), rgb)] + [a])


# Icons: A8, recoloured when drawn.
for f in sorted(os.listdir(exp)):
    if f.startswith(('g_', 'i_')) and f.endswith('.png'):
        emit('ic_' + f[2:-4] if f.startswith('g_') else 'ic_' + f[2:-4], Image.open(os.path.join(exp, f)).convert('RGBA'), a8=True)

# Car pictures. dir, lights set, geometry key, seats
CARS = [('y7', 'car', 'y', ['D', 'P', '2L', '2C', '2R', '3L', '3R']),
        ('y5', 'car5', 'y', ['D', 'P', '2L', '2C', '2R']),
        ('yl', 'carL', 'yl', ['D', 'P', '2L', '2R', '3L', '3R']),
        ('three', 'car3', 'three', ['D', 'P', '2L', '2C', '2R'])]
ELEMENTS = ['cone_low', 'cone_high', 'rear_glow', 'rear_glow_brake', 'front_glow', 'front_drl', 'front_low',
            'front_high', 'rear_line', 'rear_line_brake', 'turn_l', 'turn_r']
SEATS = ['D', 'P', '2L', '2C', '2R', '3L', '3R']
h_out.append('typedef struct { const lv_image_dsc_t *img; int16_t x, y; } ui_layer_t; /* x, y: from the car picture\'s top left */\n'
             'typedef struct {\n  const lv_image_dsc_t *car;\n  ui_layer_t drl, drl_glow;\n'
             f'  ui_layer_t seat_green[{len(SEATS)}], seat_red[{len(SEATS)}]; /* {" ".join(SEATS)} */\n'
             f'  ui_layer_t light[{len(ELEMENTS)}]; /* {" ".join(ELEMENTS)} */\n'
             '  float rows[3], axles[2], tyre_l, tyre_r, tyre_w, pill_l, pill_w, centre, path_from[2];\n} ui_car_pics_t;\n'
             'extern const ui_car_pics_t ui_car_pics[4]; /* y7 y5 yl three */\n')

lights = {}
for key in ('y', 'yl', 'three'):
    for e in ELEMENTS:
        img, x, y = cropped(Image.open(os.path.join(exp, f'{key}_{e}.png')).convert('RGBA'))
        if img is None:
            continue
        mx, my = geo[f'{key}_{e}']
        emit(f'lt_{key}_{e}', img)
        lights[key, e] = (f'&lt_{key}_{e}', round(x - mx), round(y - my))

# The approval prompt's scroll wheel (main.dart: 72 dp in the snackbar), at the panels' size.
wheel = Image.open(os.path.join(app, 'bt', 'wheel.png')).convert('RGBA')
wh = round(56 * px * geo['sizes']['panel'])
emit('img_wheel', wheel.resize((round(wheel.width * wh / wheel.height), wh), Image.LANCZOS))

tables = []
for name, d, key, seats in CARS:
    g = geo[key]
    size = (round(CAR_H * g['aspect']), CAR_H)

    def load(f):
        p = os.path.join(app, d, f)
        return Image.open(p).convert('RGBA').resize(size, Image.LANCZOS) if os.path.exists(p) else None

    emit(f'car_{name}', load('car.png'))
    lay = {}
    for f in ('drl', 'drl_glow'):
        im = load(f + '.png')
        if im is not None:
            im, x, y = cropped(im)
            emit(f'car_{name}_{f}', im)
            lay[f] = (f'&car_{name}_{f}', x, y)
    for s in seats:
        im, x, y = cropped(load(f'seat_{s}.png'))
        for col, rgb in (('green', REGEN), ('red', ALERT)):
            emit(f'car_{name}_seat_{s}_{col}', modulate(im, rgb))
            lay[s, col] = (f'&car_{name}_seat_{s}_{col}', x, y)

    def ref(t):
        return '{%s, %d, %d}' % t if t else '{0}'
    tables.append('  {.car = &car_%s, .drl = %s, .drl_glow = %s,\n   .seat_green = {%s},\n   .seat_red = {%s},\n   .light = {%s},\n'
                  '   .rows = {%s}, .axles = {%s}, .tyre_l = %gf, .tyre_r = %gf, .tyre_w = %gf, .pill_l = %gf, .pill_w = %gf,\n'
                  '   .centre = %gf, .path_from = {%gf, %gf}}' % (
                      name, ref(lay.get('drl')), ref(lay.get('drl_glow')),
                      ', '.join(ref(lay.get((s, 'green'))) for s in SEATS),
                      ', '.join(ref(lay.get((s, 'red'))) for s in SEATS),
                      ', '.join(ref(lights.get((key, e))) for e in ELEMENTS),
                      ', '.join('%gf' % v for v in g['rows']), ', '.join('%gf' % v for v in g['axles']),
                      g['tyreL'], g['tyreR'], g['tyreW'], g['pillL'], g['pillW'], g['centre'], g['pathFrom'][0], g['pathFrom'][1]))
c_out.append('const ui_car_pics_t ui_car_pics[4] = {\n%s\n};\n' % ',\n'.join(tables))

for k, v in geo['sizes'].items():  # the Dash's sizes over the app's, for dash_ui.c
    if not k.startswith('_'):
        h_out.insert(0, f'#define UI_S_{k.upper()} {float(v)}f\n')
os.makedirs(out, exist_ok=True)
hdr = '/* Generated by ui/assets/export.sh from the wattline app\'s own renders and pictures. Do not edit. */\n'
with open(os.path.join(out, 'assets.h'), 'w') as f:
    f.write(hdr + '#pragma once\n#include "lvgl.h"\n' + ''.join(h_out))
with open(os.path.join(out, 'assets.c'), 'w') as f:
    f.write(hdr + '#include "assets.h"\n' + ''.join(c_out))
print(f'car height {CAR_H} px; {len(h_out)} declarations -> {out}')
