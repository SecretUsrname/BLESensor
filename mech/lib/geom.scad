// Small 2D helpers and an involute spur gear generator. No external libraries needed.

function polar(r, a) = [r * cos(a), r * sin(a)];

// Rounded rectangle from corner (x0, y0) to (x1, y1).
module rrect(x0, y0, x1, y1, r) {
  translate([x0 + r, y0 + r]) offset(r = r) square([x1 - x0 - 2 * r, y1 - y0 - 2 * r]);
}

module sector_2d(r, a0, a1, n = 60) {
  polygon(concat([[0, 0]], [for (i = [0 : n]) polar(r, a0 + (a1 - a0) * i / n)]));
}

// Arc-shaped slot of width w, centred on radius r, whose end-circle centres sit at angles a0 and a1.
module arc_slot_2d(r, w, a0, a1) {
  union() {
    intersection() {
      difference() { circle(r = r + w / 2); circle(r = r - w / 2); }
      sector_2d(r + w, a0, a1);
    }
    translate(polar(r, a0)) circle(d = w);
    translate(polar(r, a1)) circle(d = w);
  }
}

// D-shaped bore: circle of diameter d with a flat at distance (flat - d/2) from the centre.
module d_bore_2d(d, flat) {
  intersection() {
    circle(d = d);
    translate([-d, -d]) square([d + flat - d / 2, 2 * d]);
  }
}

// ---------- Involute spur gear ----------
// Involute polar angle (radians) at roll parameter t (radians).
function _invol(t) = t - atan(t) * PI / 180;

// m = module, z = teeth, pa = pressure angle (deg), backlash = arc length removed at the pitch circle.
module involute_gear_2d(m, z, pa = 20, backlash = 0, steps = 8) {
  rp     = m * z / 2;
  rb     = rp * cos(pa);
  ra     = rp + m;
  rf     = rp - 1.25 * m;
  inv_pa = tan(pa) - pa * PI / 180;
  half_b = PI / (2 * z) + inv_pa - backlash / (2 * rp);  // half tooth angle at the base circle (rad)
  t_max  = sqrt(pow(ra / rb, 2) - 1);
  t_min  = rf > rb ? sqrt(pow(rf / rb, 2) - 1) : 0;
  deg    = 180 / PI;
  pts = [ for (i = [0 : z - 1])
            let (c = i * 360 / z)
            each concat(
              rf < rb ? [polar(rf, c - half_b * deg)] : [],
              [ for (s = [0 : steps]) let (t = t_min + (t_max - t_min) * s / steps)
                  polar(rb * sqrt(1 + t * t), c - (half_b - _invol(t)) * deg) ],
              [ for (s = [steps : -1 : 0]) let (t = t_min + (t_max - t_min) * s / steps)
                  polar(rb * sqrt(1 + t * t), c + (half_b - _invol(t)) * deg) ],
              rf < rb ? [polar(rf, c + half_b * deg)] : []
            ) ];
  polygon(pts);
}
