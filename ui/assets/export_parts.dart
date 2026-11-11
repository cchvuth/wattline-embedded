// Added to a scratch copy of the app as a part of main.dart (ui/assets/export.sh), so it can
// reach the private car-lights painter. Paints one element of it at a time: a canvas that
// passes on only the draws whose paint the element uses.
part of 'main.dart';

class _Tap implements Canvas {
  _Tap(this.c, this.keep);
  final Canvas c;
  final bool Function(Paint) keep;

  @override
  void drawPath(Path path, Paint paint) {
    if (keep(paint)) c.drawPath(path, paint);
  }

  @override
  void drawCircle(Offset o, double r, Paint paint) {} // the steering path dots: not exported

  @override
  dynamic noSuchMethod(Invocation i) => throw UnsupportedError('${i.memberName}');
}

bool _red(Paint p) => p.shader == null && p.color.r > 0.8 && p.color.g < 0.5;
bool _amber(Paint p) => p.shader == null && p.color.r > 0.9 && p.color.g > 0.6 && p.color.b < 0.6;
bool _white(Paint p) => p.shader == null && p.color.r > 0.8 && p.color.g > 0.8 && p.color.b > 0.8;
bool _cone(Paint p) => p.shader != null;

/// One car-lights element, painted on a [w] x [h] car (logical px) placed [mx], [my] in.
void exportLights(Canvas canvas, Car car, String element, double w, double h) {
  final (beams, keep, drl, low, high, brake, left, right) = switch (element) {
    'cone_low' => (true, _cone, false, true, false, false, false, false),
    'cone_high' => (true, _cone, false, false, true, false, false, false),
    'rear_glow' => (true, _red, true, false, false, false, false, false),
    'rear_glow_brake' => (true, _red, false, false, false, true, false, false),
    'front_glow' => (true, _white, true, false, false, false, false, false),
    'front_drl' => (false, _white, true, false, false, false, false, false),
    'front_low' => (false, _white, false, true, false, false, false, false),
    'front_high' => (false, _white, false, false, true, false, false, false),
    'rear_line' => (false, _red, true, false, false, false, false, false),
    'rear_line_brake' => (false, _red, false, false, false, true, false, false),
    'turn_l' => (false, _amber, false, false, false, false, true, false),
    'turn_r' => (false, _amber, false, false, false, false, false, true),
    _ => throw ArgumentError(element),
  };
  _CarLights(car: car, drl: drl, low: low, high: high, brake: brake, left: left, right: right, beams: beams)
      .paint(_Tap(canvas, keep), Size(w, h));
}

/// The picture's aspect (width / height), as the app draws it.
double exportAspect(Car car) => _Picture.of(car).aspect;

/// Where the cabin's figures go on a car picture (shares of its width and length).
Map<String, Object> exportGeometry(Car car) {
  final p = _Picture.of(car);
  return {
    'aspect': p.aspect,
    'rows': p.rows,
    'axles': p.axles,
    'tyreL': p.tyreL,
    'tyreR': p.tyreR,
    'tyreW': p.tyreW,
    'pillL': p.pillL,
    'pillW': p.pillW,
    'centre': p.centre,
    'pathFrom': [p.pathFrom.$1, p.pathFrom.$2],
    'dir': p.dir,
  };
}
