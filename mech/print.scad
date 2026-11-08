// Print-ready single part, placed flat on the bed. Pick one with -D part="name".
// Parts: base hub out_gear pinion bridge lid knob
// Fit tests: fit_tp_width fit_tp_thick fit_screw_gauge fit_hub_slot

include <parts.scad>
include <fit_tests.scad>

part = "base";

if      (part == "base")            base();
else if (part == "hub")             translate([0, 0, -hub_z0]) hub();
else if (part == "out_gear")        translate([0, 0, -gear_z0]) out_gear();
else if (part == "pinion")          translate([0, -centre_dist, -gear_z0]) pinion();
else if (part == "bridge")          translate([0, 0, -bridge_z0]) bridge();
else if (part == "lid")             rotate([180, 0, 0]) translate([0, 0, -lid_z1]) lid();
else if (part == "knob")            rotate([180, 0, 0]) translate([0, 0, -(hub_z1 + knob_gap + knob_h)]) knob();
else if (part == "fit_tp_width")    fit_tp_width();
else if (part == "fit_tp_thick")    fit_tp_thick();
else if (part == "fit_screw_gauge") fit_screw_gauge();
else if (part == "fit_hub_slot")    fit_hub_slot();
else assert(false, str("unknown part: ", part));
