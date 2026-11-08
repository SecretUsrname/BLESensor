// Assembly view. Set explode > 0 to pull the parts apart along Z.
// show_* flags toggle parts so you can look inside.

include <parts.scad>

explode     = 0;      // try 15
show_base   = true;
show_lid    = false;
show_knob   = true;
show_motor  = true;   // grey placeholder for the N20 gearmotor

if (show_base)  color("SteelBlue", 0.55) base();
color("Orange")      translate([0, 0, 1 * explode]) hub();
color("Gold")        translate([0, 0, 2 * explode]) out_gear();
color("Goldenrod")   translate([0, 0, 2 * explode]) pinion();
color("SeaGreen")    translate([0, 0, 3 * explode]) bridge();
if (show_motor) color("Gray") translate([0, centre_dist, bridge_z1 + 3 * explode])
  translate([-motor_w / 2, -motor_h / 2, 0]) cube([motor_w, motor_h, motor_len]);
if (show_lid)  color("WhiteSmoke", 0.6) translate([0, 0, 4 * explode]) lid();
if (show_knob) color("Tomato")         translate([0, 0, 5 * explode]) knob();
