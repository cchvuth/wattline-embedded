// Run by ui/assets/export.sh in a scratch copy of the app: renders the app's own icons (its
// glyph painters and the Material icons it uses) and its car-lights painter, element by
// element, at the Dash's pixel sizes, white (or in their own colours) on transparent.
// OUT=<dir>: PNGs plus geometry.json. The Dash draws them as images (A8 icons recoloured).
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tesla_dash/car.dart';
import 'package:tesla_dash/glyphs.dart';
import 'package:tesla_dash/main.dart';

const px = 1.25; // px per dp on the Dash
// The Dash's sizes over the app's (ui/assets/sizes.json).
final Map<String, dynamic> sz = jsonDecode(File(Platform.environment['SIZES']!).readAsStringSync());
double S(String k) => (sz[k] as num).toDouble();
final cab = S('car_h') / (236 * px); // the cabin's car, 236 dp in the app
final dial = 128 / 104 * S('dial'); // the battery dial shown at 128 dp, drawn at 104

// name, widget, size (dp), px per dp
final icons = <(String, Widget Function(double), double, double)>[
  for (final (n, g) in [
    ('trunk', Glyph.trunk), ('mirror', Glyph.mirrorTilt), ('door_fl', Glyph.doorFL), ('door_rl', Glyph.doorRL),
    ('charge_port', Glyph.chargePort), ('frunk', Glyph.frunk), ('recirc', Glyph.recirc),
    ('door_fr', Glyph.doorFR), ('door_rr', Glyph.doorRR), ('interior', Glyph.interiorLight),
  ])
    ('g_$n', (s) => GlyphIcon(g, size: s, color: Colors.white), 24, px * S('chip')),
  ('g_recirc_s', (s) => GlyphIcon(Glyph.recirc, size: s, color: Colors.white), 22, px * S('chip')),
  for (final (n, g) in [('drl', Glyph.drl), ('low', Glyph.lowBeam), ('high', Glyph.highBeam), ('grip', Glyph.grip)])
    ('g_$n', (s) => GlyphIcon(g, size: s, color: Colors.white), 20, px * S('lights')),
  ('g_autopilot', (s) => GlyphIcon(Glyph.autopilot, size: s, color: Colors.white), 30, px * S('ring')),
  ('g_cruise', (s) => GlyphIcon(Glyph.cruise, size: s, color: Colors.white), 30 * 0.66, px * S('ring')),
  ('g_washer', (s) => GlyphIcon(Glyph.washer, size: s, color: Colors.white), 15, px * S('ring')),
  ('g_fan', (s) => GlyphIcon(Glyph.fan, size: s, color: Colors.white), 12, px * cab),
  ('g_fan_off', (s) => GlyphIcon(Glyph.fanOff, size: s, color: Colors.white), 12, px * cab),
  ('i_bolt_dial', (s) => Icon(Icons.bolt, size: s, color: Colors.white), 17, px * dial),
  ('i_snow_dial', (s) => Icon(Icons.ac_unit, size: s, color: Colors.white), 14, px * dial),
  ('i_up_dial', (s) => Icon(Icons.arrow_upward, size: s, color: Colors.white), 14, px * dial),
  for (final (n, i) in [
    ('hourglass', Icons.hourglass_bottom), ('flag', Icons.flag), ('route', Icons.route), ('bolt', Icons.bolt),
    ('ecar', Icons.electric_car), ('battery', Icons.battery_std), ('place', Icons.place),
  ])
    ('i_$n', (s) => Icon(i, size: s, color: Colors.white), 15, px * S('nav')),
  // The Android Auto button sits beside the car-panel button: same size and scale as i_car_panel (36 px), so it
  // renders crisp with no runtime upscale (i_route above: the destination box's row icon).
  ('i_route_btn', (s) => Icon(Icons.route, size: s, color: Colors.white), 24, px * S('link')),
  ('i_gap_car', (s) => Icon(Icons.directions_car_filled, size: s, color: Colors.white), 16, px * S('ring')),
  ('i_wifi_s', (s) => Icon(Icons.wifi, size: s, color: Colors.white), 18, px * S('link')),
  ('i_bt_s', (s) => Icon(Icons.bluetooth_connected, size: s, color: Colors.white), 18, px * S('link')),
  for (final (n, i) in [
    ('wifi', Icons.wifi), ('wifi_off', Icons.wifi_off), ('wifi_find', Icons.wifi_find),
    ('bt', Icons.bluetooth_connected), ('bt_off', Icons.bluetooth_disabled), ('bt_find', Icons.bluetooth_searching),
    ('bt_plain', Icons.bluetooth),
  ])
    ('i_${n}_l', (s) => Icon(i, size: s, color: Colors.white), 22, px * S('link')),
  ('i_car_panel', (s) => Icon(Icons.directions_car_outlined, size: s, color: Colors.white), 24, px * S('link')),
  // The car panel (car_panel.dart: OutlinedButton.icon, 18 dp) and the settings, at the panels' size.
  for (final (n, i) in [
    ('lock_open', Icons.lock_open), ('lock', Icons.lock), ('climate', Icons.ac_unit), ('flash', Icons.flashlight_on),
    ('honk', Icons.campaign), ('add', Icons.add), ('net', Icons.wifi), ('fixed', Icons.lock_outline),
  ])
    ('i_p_$n', (s) => Icon(i, size: s, color: Colors.white), 18, px * S('panel')),
  for (final (n, i) in [('back', Icons.arrow_back), ('close', Icons.close), ('delete', Icons.delete_outline)])
    ('i_p_$n', (s) => Icon(i, size: s, color: Colors.white), 24, px * S('panel')),
];

final cars = {'y': Car.modelY7, 'yl': Car.modelYL, 'three': Car.model3};
const elements = [
  'cone_low', 'cone_high', 'rear_glow', 'rear_glow_brake', 'front_glow',
  'front_drl', 'front_low', 'front_high', 'rear_line', 'rear_line_brake', 'turn_l', 'turn_r',
];

Future<void> png(ui.Image img, String path) async {
  final bytes = await img.toByteData(format: ui.ImageByteFormat.png);
  File(path).writeAsBytesSync(bytes!.buffer.asUint8List());
}

void main() {
  final out = Platform.environment['OUT'] ?? '/tmp/ui_assets';
  testWidgets('export icons', (t) async {
    Directory(out).createSync(recursive: true);
    for (final (name, make, size, k) in icons) {
      final key = GlobalKey();
      await t.pumpWidget(Directionality(
          textDirection: TextDirection.ltr,
          child: Center(child: RepaintBoundary(key: key, child: SizedBox.square(dimension: size, child: make(size))))));
      await t.runAsync(() async {
        final img = await (key.currentContext!.findRenderObject() as RenderRepaintBoundary).toImage(pixelRatio: k);
        await png(img, '$out/$name.png');
      });
    }
  });

  testWidgets('export car lights', (t) async {
    final geo = <String, Object>{'cab': cab, 'px': px, 'sizes': sz};
    await t.runAsync(() async {
      for (final MapEntry(key: name, value: car) in cars.entries) {
        final g = exportGeometry(car);
        geo[name] = g;
        const h = 236.0; // the cabin's car, dp, before the column's fit
        final w = h * (g['aspect'] as double), k = px * cab;
        final mx = w * 0.6, my = h * 0.5, mb = h * 0.15; // room past the picture for the beams and glows
        for (final e in elements) {
          final rec = ui.PictureRecorder();
          final c = Canvas(rec)
            ..scale(k)
            ..translate(mx, my);
          exportLights(c, car, e, w, h);
          final img = await rec.endRecording().toImage(((w + 2 * mx) * k).ceil(), ((h + my + mb) * k).ceil());
          await png(img, '$out/${name}_$e.png');
          geo['${name}_$e'] = [mx * k, my * k];
        }
      }
    });
    File('$out/geometry.json').writeAsStringSync(const JsonEncoder.withIndent(' ').convert(geo));
  });
}
