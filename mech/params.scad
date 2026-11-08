// ===== SLock parametric design: shared parameters =====
// Units: mm and degrees.
// Frame: origin = spindle (tailpiece) centre, Z = spindle axis pointing into the room,
//        z = 0 is the door surface, +Y = up (towards the ceiling).
// Values marked [UNVERIFIED] are estimates or retailer figures. Confirm them with the
// fit-test coupons (fit_tests.scad) before ordering the real parts.

$fn = 72;
eps = 0.01;

// ---------- Lock interface: Kwikset 660-family deadbolt ----------
rose_d        = 63.5;  // [UNVERIFIED] original thumb-turn rose diameter (2-1/2")
door_plate_d  = 50;    // [UNVERIFIED] plate left on the door once the thumb turn is off
door_plate_h  = 2.5;   // [UNVERIFIED] how far that plate stands off the door
screw_spacing = 42;    // [UNVERIFIED] centre-to-centre of the 2 mounting screws
screw_angle   = 90;    // [UNVERIFIED] 90 = screws above/below the spindle, 0 = left/right
screw_clear_d = 5.0;   // clears #8-32 and #10-24 machine screws
screw_head_d  = 10.0;
screw_cbore   = 1.5;   // counterbore depth for the screw heads
tp_w          = 8.0;   // [UNVERIFIED] tailpiece width
tp_t          = 2.4;   // [UNVERIFIED] tailpiece thickness
tp_clear      = 0.35;  // clearance added to each slot dimension
tp_slot_depth = 19;    // [UNVERIFIED] must be >= tailpiece length beyond the door plate
tp_slot_angle = 0;     // slot long axis angle in the hub (0 = along X)
travel        = 90;    // [UNVERIFIED] rotation from locked to unlocked

// ---------- Drive: motor pinion -> output gear (lost motion) -> hub ----------
gear_m        = 1.25;  // module
gear_pa       = 20;    // pressure angle
z_out         = 40;    // output gear teeth
z_pin         = 12;    // pinion teeth
gear_t        = 7;     // face width
gear_backlash = 0.15;
centre_extra  = 0.2;   // extra centre distance for print tolerance
centre_dist   = gear_m * (z_out + z_pin) / 2 + centre_extra;
ratio         = z_out / z_pin;

lug_r         = 12;    // radius of the hub lug that rides in the gear's arc slot
lug_d         = 3.5;
lug_clear     = 0.6;
deadband      = 6;     // free play past each end of travel when the gear is parked

// ---------- Hub ----------
hub_d         = 14;
hub_clear     = 0.5;   // bores = hub_d + hub_clear
flange_d      = 32;
flange_t      = 3;
hex_af        = 9;     // hex for the manual knob (across flats)
hex_len       = 10;
magnet_d      = 4;     // axially magnetised 4 x 8 mm cylinder in a radial cross-hole
magnet_len    = 8;

// ---------- Motor: N20 gearmotor with hall encoder ----------
motor_w          = 12;   // gearbox width  (X)
motor_h          = 10;   // gearbox height (Y)
motor_len        = 34;   // [UNVERIFIED] gearbox + motor + encoder, excluding shaft
motor_shaft_d    = 3;
motor_shaft_flat = 2.5;  // across-flat of the D shaft
motor_shaft_len  = 10;   // [UNVERIFIED]
motor_hole_sp    = 9;    // [UNVERIFIED] M1.6 holes on the gearbox face
motor_clamp_h    = 9;
pinion_boss_d    = 5.5;  // must clear the M1.6 face holes

// ---------- Housing ----------
housing_w     = 81;
housing_y0    = -32;    // bottom edge (below the spindle)
housing_y1    = 108.5;  // top edge
corner_r      = 8;
wall          = 2.5;
floor_t       = 6;
lid_t         = 2.5;
batt_w        = 62;     // [UNVERIFIED] flat 4xAA holder footprint
batt_l        = 58;
batt_y0       = 47.5;

// ---------- Hub angle sensors: 2x DRV5055 linear hall, TO-92 ----------
sense_r       = 8.6;    // inner face radius of the sensor pockets
sense_angles  = [180, 270];

// ---------- Derived Z stack ----------
hub_z0     = door_plate_h + 0.5;
flange_z0  = floor_t + 0.3;
flange_z1  = flange_z0 + flange_t;
gear_z0    = flange_z1 + 0.5;
gear_z1    = gear_z0 + gear_t;
bridge_t   = 3;
bridge_z0  = gear_z1 + 0.8;
bridge_z1  = bridge_z0 + bridge_t;
motor_z1   = bridge_z1 + motor_len;
lid_z0     = motor_z1 + 1.5;
lid_z1     = lid_z0 + lid_t;
hub_z1     = lid_z1;              // top of the round shaft; hex starts here
magnet_z   = bridge_z1 + 5.5;     // centre of magnet cross-hole and sensors
knob_gap   = 0.5;
knob_h     = 16;

// ---------- Fastener positions ----------
standoffs  = [[31, -8], [-31, -8], [31, 38], [-31, 38]];          // bridge, M3
lid_posts  = [[35, -26.5], [-35, -26.5], [35, 103], [-35, 103]];  // lid, M3

// ---------- Sanity checks ----------
assert(hub_z0 + tp_slot_depth < magnet_z - magnet_d / 2 - 1, "tailpiece slot runs into the magnet hole");
assert(screw_spacing / 2 - screw_head_d / 2 > flange_d / 2 - 2 || screw_spacing / 2 < 0, "screw heads clash with hub flange");
assert(centre_dist - gear_m * (z_pin / 2 + 1) > flange_d / 2, "pinion clashes with hub flange");
assert(lug_r - lug_d / 2 > hub_d / 2 + 1, "lug too close to hub shaft");
