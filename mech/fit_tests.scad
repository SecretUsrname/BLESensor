// Cheap PLA fit-test coupons. Print these first to measure the lock without calipers.
// This file only defines modules; export them through print.scad.

include <params.scad>
include <lib/geom.scad>

label_font = "Liberation Sans:style=Bold";

module _label(s, size) {
  linear_extrude(1) text(s, size = size, halign = "center", valign = "center", font = label_font);
}

// Slide each slot over the tailpiece end. The tightest slot that still fits = tailpiece width.
module fit_tp_width() {
  ws = [for (w = [6.0 : 0.25 : 10.0]) w];
  cols = 6; cw = 15; ch = 14; t = 5;
  rows = ceil(len(ws) / cols);
  difference() {
    linear_extrude(t) rrect(0, 0, cols * cw, rows * ch, 2);
    for (i = [0 : len(ws) - 1]) let (c = i % cols, r = floor(i / cols))
      translate([c * cw + cw / 2, r * ch + 8.5, 0]) {
        translate([0, 0, -eps]) linear_extrude(t + 1) square([ws[i], 4.0], center = true);
        translate([0, -6, t - 0.6]) _label(str(ws[i]), 2.6);
      }
  }
}

// Same idea for thickness: slots are wide, only the thickness varies.
module fit_tp_thick() {
  ts = [for (x = [1.6 : 0.2 : 3.6]) x];
  cols = 4; cw = 18; ch = 12; t = 5;
  rows = ceil(len(ts) / cols);
  difference() {
    linear_extrude(t) rrect(0, 0, cols * cw, rows * ch, 2);
    for (i = [0 : len(ts) - 1]) let (c = i % cols, r = floor(i / cols))
      translate([c * cw + cw / 2, r * ch + 7.5, 0]) {
        translate([0, 0, -eps]) linear_extrude(t + 1) square([11, ts[i]], center = true);
        translate([0, -5, t - 0.6]) _label(str(ts[i]), 2.4);
      }
  }
}

// Lay this disc over the lock with the thumb turn removed, tailpiece through the centre hole.
// Turn it until the screw holes show through both slots, then read the spacing on the scale.
// The outer edge is the published rose diameter; arcs on the left measure the door plate.
module fit_screw_gauge() {
  t = 2.4;
  difference() {
    cylinder(d = rose_d, h = t);
    translate([0, 0, -eps]) cylinder(d = 20, h = t + 1);
    for (s = [-1, 1]) hull() {
      translate([0, s * 13, -eps]) cylinder(d = screw_clear_d, h = t + 1);
      translate([0, s * 28, -eps]) cylinder(d = screw_clear_d, h = t + 1);
    }
    // spacing scale (centre-to-centre spacing = 2 x radius), ticks every 2 mm of spacing
    for (sp = [26 : 2 : 56]) for (s = [-1, 1]) {
      translate([screw_clear_d / 2 + 0.6, s * sp / 2 - 0.2, t - 0.5]) cube([sp % 4 == 0 ? 3.5 : 2, 0.4, 1]);
    }
    for (sp = [28 : 4 : 56]) translate([screw_clear_d / 2 + 6.5, sp / 2, t - 0.5]) _label(str(sp), 2);
    // door-plate diameter arcs on the left half
    for (d = [40 : 4 : 60]) {
      translate([0, 0, t - 0.5]) linear_extrude(1) intersection() {
        difference() { circle(d = d + 0.4); circle(d = d - 0.4); }
        translate([-40, -6]) square([33, 12]);
      }
    }
    for (d = [40 : 8 : 56]) translate([-d / 2 - 1.6, 8, t - 0.5]) rotate(90) _label(str(d), 1.8);
  }
}

// Short hub section with the slot size currently set in params.scad. Final check before the real hub.
module fit_hub_slot() {
  difference() {
    cylinder(d = 20, h = 12);
    translate([0, 0, -eps]) linear_extrude(13) square([tp_w + tp_clear, tp_t + tp_clear], center = true);
    translate([0, 5.5, 12 - 0.6]) _label(str(tp_w, "x", tp_t), 2.4);
  }
}
