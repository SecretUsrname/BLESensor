// SLock v0 parts, modelled in place (assembly coordinates). See params.scad for the frame.
// This file only defines modules; open assembly.scad to view or print.scad to export.

include <params.scad>
include <lib/geom.scad>

module housing_2d() {
  rrect(-housing_w / 2, housing_y0, housing_w / 2, housing_y1, corner_r);
}

// ---------- Base tray: screws to the lock with the 2 original screws ----------
module batt_ribs_2d() {
  difference() {
    offset(delta = 2.5) rrect(-batt_w / 2 - 0.5, batt_y0 - 0.5, batt_w / 2 + 0.5, batt_y0 + batt_l + 0.5, 1);
    rrect(-batt_w / 2 - 0.5, batt_y0 - 0.5, batt_w / 2 + 0.5, batt_y0 + batt_l + 0.5, 1);
  }
}

module base() {
  difference() {
    union() {
      linear_extrude(floor_t) housing_2d();
      linear_extrude(lid_z0) difference() { housing_2d(); offset(delta = -wall) housing_2d(); }
      for (p = lid_posts) translate([p[0], p[1], 0]) cylinder(d = 6, h = lid_z0);
      for (p = standoffs) translate([p[0], p[1], 0]) cylinder(d = 7, h = bridge_z0);
      intersection() {
        translate([0, 0, floor_t - eps]) linear_extrude(3) batt_ribs_2d();
        linear_extrude(lid_z0) housing_2d();
      }
    }
    // spindle bore
    translate([0, 0, -eps]) cylinder(d = hub_d + hub_clear, h = floor_t + 1);
    // pocket for the plate that stays on the door
    translate([0, 0, -eps]) cylinder(d = door_plate_d + 1, h = door_plate_h + 0.3);
    // the 2 original mounting screws
    for (s = [-1, 1]) rotate(screw_angle) translate([s * screw_spacing / 2, 0, 0]) {
      translate([0, 0, -eps]) cylinder(d = screw_clear_d, h = floor_t + 1);
      translate([0, 0, floor_t - screw_cbore]) cylinder(d = screw_head_d, h = screw_cbore + 1);
    }
    // self-tapping M3 pilot holes
    for (p = lid_posts) translate([p[0], p[1], lid_z0 - 12]) cylinder(d = 2.7, h = 13);
    for (p = standoffs) translate([p[0], p[1], bridge_z0 - 10]) cylinder(d = 2.7, h = 11);
  }
}

// ---------- Hub: takes the tailpiece, carries the lug, magnet and manual knob ----------
module hub() {
  sw = tp_w + tp_clear;
  st = tp_t + tp_clear;
  difference() {
    union() {
      translate([0, 0, hub_z0]) cylinder(d = hub_d, h = hub_z1 - hub_z0);
      translate([0, 0, flange_z0]) cylinder(d = flange_d, h = flange_t);
      translate([lug_r, 0, flange_z1 - eps]) cylinder(d = lug_d, h = gear_z1 - 0.3 - flange_z1);
      translate([0, 0, hub_z1 - eps]) linear_extrude(hex_len) circle(d = hex_af / cos(30), $fn = 6);
    }
    // tailpiece slot with lead-in chamfer
    translate([0, 0, hub_z0 - eps]) rotate(tp_slot_angle) {
      linear_extrude(tp_slot_depth) square([sw, st], center = true);
      linear_extrude(height = 1.2, scale = [sw / (sw + 2), st / (st + 2)]) square([sw + 2, st + 2], center = true);
    }
    // radial cross-hole for the 4 x 8 mm magnet (magnetised across the hub axis)
    translate([0, 0, magnet_z]) rotate([0, 90, 0]) cylinder(d = magnet_d + 0.2, h = hub_d + 2, center = true);
    // M3 pilot for the knob screw
    translate([0, 0, hub_z1 + hex_len - 10]) cylinder(d = 2.7, h = 10 + eps);
  }
}

// ---------- Output gear with lost-motion arc slot ----------
module out_gear() {
  translate([0, 0, gear_z0]) linear_extrude(gear_t) difference() {
    involute_gear_2d(gear_m, z_out, gear_pa, gear_backlash);
    circle(d = hub_d + hub_clear);
    arc_slot_2d(lug_r, lug_d + lug_clear, -deadband, travel + deadband);
  }
}

// ---------- Motor pinion (press-fit on the N20 D-shaft) ----------
module pinion() {
  translate([0, centre_dist, 0]) difference() {
    union() {
      // rotated half a tooth so it meshes with the output gear in the assembly view
      translate([0, 0, gear_z0]) linear_extrude(gear_t) rotate(180 / z_pin) involute_gear_2d(gear_m, z_pin, gear_pa, gear_backlash);
      translate([0, 0, gear_z1 - eps]) cylinder(d = pinion_boss_d, h = bridge_z1 - 1.2 - gear_z1);
    }
    translate([0, 0, gear_z0 - eps]) linear_extrude(bridge_z1) d_bore_2d(motor_shaft_d + 0.1, motor_shaft_flat + 0.1);
  }
}

// ---------- Bridge: holds the motor, the hub's upper bearing and the hall sensors ----------
module bridge() {
  difference() {
    union() {
      translate([0, 0, bridge_z0]) linear_extrude(bridge_t) rrect(-35, -15, 35, 45, 4);
      // motor clamp around the gearbox
      translate([0, centre_dist, bridge_z1 - eps]) linear_extrude(motor_clamp_h) difference() {
        square([motor_w + 0.3 + 3.2, motor_h + 0.3 + 3.2], center = true);
        square([motor_w + 0.3, motor_h + 0.3], center = true);
      }
      // sensor blocks
      for (a = sense_angles) rotate(a) translate([sense_r - 0.6, -3.5, bridge_z1 - eps])
        cube([5, 7, magnet_z + 4.5 - bridge_z1]);
    }
    translate([0, 0, bridge_z0 - 1]) cylinder(d = hub_d + hub_clear, h = 50);
    translate([0, centre_dist, bridge_z0 - 1]) cylinder(d = pinion_boss_d + 0.7, h = bridge_t + 2);
    for (s = [-1, 1]) translate([s * motor_hole_sp / 2, centre_dist, bridge_z0 - 1]) cylinder(d = 1.9, h = bridge_t + 2);
    for (p = standoffs) translate([p[0], p[1], bridge_z0 - 1]) {
      cylinder(d = 3.4, h = bridge_t + 2);
      translate([0, 0, bridge_t + 1 - 1.8]) cylinder(d = 6.2, h = 2);
    }
    // TO-92 pockets, open at the top, flat face towards the hub
    for (a = sense_angles) rotate(a) translate([sense_r, -2.2, magnet_z - 1.8]) cube([1.8, 4.4, 20]);
  }
}

// ---------- Lid ----------
module lid() {
  difference() {
    union() {
      translate([0, 0, lid_z0]) linear_extrude(lid_t) housing_2d();
      translate([0, 0, lid_z0 - 2]) linear_extrude(2 + eps) difference() {
        offset(delta = -wall - 0.3) housing_2d();
        offset(delta = -wall - 0.3 - 1.6) housing_2d();
      }
    }
    for (p = lid_posts) translate([p[0], p[1], lid_z0 - 3]) {
      cylinder(r = 4.5, h = 3 + eps);
      cylinder(d = 3.4, h = 10);
      translate([0, 0, 3 + lid_t - 1.6]) cylinder(d1 = 3.4, d2 = 6.6, h = 1.6 + eps);
    }
    translate([0, 0, lid_z0 - 3]) cylinder(d = hub_d + hub_clear, h = 10);
  }
}

// ---------- Manual knob (replaces the thumb turn for turning by hand) ----------
module knob() {
  z0 = hub_z1 + knob_gap;
  translate([0, 0, z0]) difference() {
    hull() for (s = [-1, 1]) translate([s * 15, 0, 0]) cylinder(d = 14, h = knob_h);
    translate([0, 0, -eps]) linear_extrude(hex_len - knob_gap + 0.2) circle(d = (hex_af + 0.3) / cos(30), $fn = 6);
    cylinder(d = 3.4, h = knob_h + 1);
    translate([0, 0, knob_h - 3.2]) cylinder(d = 6.2, h = 4);
  }
}
