// Surface-mount case for the Waveshare ESP32-S3-Touch-LCD-3.5, for mounting
// with a hook-and-loop (Velcro) strip on its flat back. No screws needed.
//
// Two printed parts:
//   bezel  the front and sides, a shell open at the back. Its lip sits over the
//          glass and its walls run all the way back to the panel.
//   plate  the back. It latches into the back of the bezel, flush with its rim.
//          The board's four brass standoffs sit on the plate, so the board is
//          held between the plate and the lip.
//
// Latches: four tongues are cut into the bezel's long walls, free at the back
// end. Each has a hook on its inside that rides over the plate's edge (the
// tongue flexes outwards) and drops into a notch, under a square ledge. Pulling
// the bezel off can't push a tongue outwards, so it holds until released.
//
// Fitting order: feed the USB-C cable through the plate's opening and plug it
// into the right-angle adapter on the board. Drop the board face down into the
// bezel, then press the plate in over it, standoffs into the guide rings,
// until all four latches click. To open it, take it off the Velcro, then for
// each latch put a small flat screwdriver into its notch from the back and
// lever the tongue's hook outwards while easing the plate out.
//
// Coordinates: looking at the screen, x across the long side, y along the short
// side, z out of the panel. The USB-C port is on the +x side. PWR/RESET/BOOT
// sit on the +y edge, near the USB end. Board figures come from Waveshare's
// outline drawing; the ones marked "est." were scaled from that drawing, so
// check them against your board.
//
//   openscad -D 'part="bezel"' -o bezel.stl case.scad    (and "plate")

part = "assembly";   // "assembly" | "exploded" | "section" | "bezel" | "plate"

/* [Board] */
glass_w   = 92.44;   // cover glass outline
glass_h   = 61.00;
glass_r   = 6.0;
stack     = 11.5;    // glass front to the tips of the back standoffs
holes     = [72.0, 48.5];        // standoff spacing, centred on the glass
usb_y     = 0.4;     // est. USB-C centre, from the glass centre (+y = buttons edge)
usb_axis  = 9.1;     // est. USB-C axis depth below the glass front
btn_from_usb_edge = [21.22, 29.72, 38.22];  // PWR, RESET, BOOT
btn_axis  = 9.25;    // est. button depth below the glass front
header    = [-22.3, 21.8, -28.0, -19.6]; // 2x16 pin header outline plus margin: x0, x1, y0, y1

/* [Bezel] */
gap       = 0.3;     // glass edge to bezel wall, each side
skirt     = 2.4;     // bezel side wall
lip_t     = 1.4;     // bezel front lip over the glass
lip_over  = 1.5;     // how far the lip overlaps the glass border
stack_clr = 0.1;     // slack between glass and lip; fill with foam tape if loose
edge_ch   = 0.8;     // chamfer on the bezel's front edge

/* [Plate] */
floor_t   = 3.5;     // plate thickness in the middle; the pin header pokes
                     //   through a cut-out, so the board can sit right on it
edge_t    = 4.5;     // and round the edge, a stiffening rim that takes the latches
rim_w     = 2.5;     // width of that rim
plate_gap = 0.15;    // plate edge to bezel wall
top_ch    = 0.6;     // chamfer on the plate's inner edge, leads it past the hooks
velcro    = [50.8, 25.4];  // recess in the back for a Velcro patch, 2" x 1"
velcro_d  = 2.5;     // its depth, leaving 1 mm of plate over it. Printed back
                     //   down, its ceiling is a bridge across the 1" side,
                     //   which prints cleanly enough; any sag only makes the
                     //   recess a touch shallower
velcro_x  = -14;     // centred between the left end and the USB opening, so the
                     //   cable doesn't pass through the patch
board_lift = 0.0;    // pads under the standoffs, if the header needs more room
ring_id   = 4.6;     // guide rings around the standoffs
ring_od   = 6.6;
ring_h    = 3.5;     // above the standoff seat; the standoffs are 4 mm
ring_low  = [[1, -1], [-1, 1]];  // rings kept low to miss the RTC battery socket
ring_low_h = 1.0;                //   (USB end, header side) and the SD card slot

/* [Latches] */
latch_x   = [-32, 32];  // positions along each long wall (clear of the buttons)
latch_w   = 6;       // tongue width
latch_h   = 14;      // tongue length, up from the back; longer flexes easier
latch_slot = 0.8;    // slits either side of the tongue
hook      = 1.0;     // how far the hook reaches in from the wall
hook_z    = 0.3;     // hook's lower end, above the back
hook_top  = 1.6;     // its square top face, the one that holds
latch_clr = 0.15;    // play around the hook in its notch

/* [Openings] */
usb_hole  = [30.5, 16]; // cable opening in the plate at the USB end, a U open to
                        //   the plate's edge for the right-angle adapter: length
                        //   in from the edge, width
usb_hole_r = 2;         // corner rounding of that opening
btn_d     = 2.2;     // pin holes for PWR / RESET / BOOT (0 to omit)

$fn = 64;
eps = 0.01;

seat  = floor_t + board_lift;                // standoff tips
glass_z = seat + stack;                      // glass front, above the panel
total = glass_z + stack_clr + lip_t;         // overall depth off the panel
d_out = gap + skirt;                         // outer outline, from the glass
out_w = glass_w + 2 * d_out;
out_h = glass_h + 2 * d_out;
wall_y = glass_h / 2 + gap;                  // bezel inner wall, long sides
pl_x  = glass_w / 2 + gap - plate_gap;       // plate edge, short sides
usb_z = glass_z - usb_axis;
btn_z = glass_z - btn_axis;

echo(str("Case: ", out_w, " x ", out_h, " x ", total, " mm"));

// rounded rectangle of the glass outline, grown by d
module outline(d = 0) {
    r = glass_r + d;
    offset(r = r) square([glass_w + 2 * d - 2 * r, glass_h + 2 * d - 2 * r], center = true);
}

// latch hook cross-section: n = distance in from the bezel wall, z up from the
// back. A 45 degree ramp underneath to ride over the plate, a square top.
module hook2d() {
    polygon([[-eps, hook_z], [hook, hook_z + hook], [hook, hook_top], [-eps, hook_top]]);
}

// the notch in the plate's edge a hook drops into; open to the back, which is
// where a screwdriver goes in to release it
module notch2d() {
    polygon([[-1, -1], [hook + latch_clr, -1], [hook + latch_clr, hook_top + 0.1], [-1, hook_top + 0.1]]);
}

// a 2D profile in (n, z), run along x from x0 for len, on the long wall at sy
module along_wall(x0, len, sy) {
    translate([x0, sy * wall_y, 0]) mirror([0, sy > 0 ? 1 : 0, 0])
        rotate([90, 0, 90]) linear_extrude(len) children();
}

module bezel() {
    difference() {
        hull() {
            linear_extrude(total - edge_ch) outline(d_out);
            linear_extrude(total) outline(d_out - edge_ch);
        }
        // cavity, open at the back
        translate([0, 0, -eps]) linear_extrude(total - lip_t + eps) outline(gap);
        // screen window, chamfered towards the front
        translate([0, 0, total - lip_t - eps]) hull() {
            linear_extrude(eps) outline(-lip_over);
            translate([0, 0, lip_t]) linear_extrude(2 * eps) outline(-lip_over + 0.5);
        }
        // button pin holes
        if (btn_d > 0) for (bx = btn_from_usb_edge)
            translate([glass_w / 2 - bx, glass_h / 2, btn_z])
                rotate([-90, 0, 0]) cylinder(d = btn_d, h = 10, $fn = 24);
        // slits either side of each latch tongue, rounded at the top end
        for (x = latch_x, sy = [-1, 1], s = [-1, 1])
            translate([x + s * (latch_w + latch_slot) / 2, sy * (wall_y + skirt / 2), 0]) hull() {
                translate([-latch_slot / 2, -skirt, -eps]) cube([latch_slot, 2 * skirt, eps]);
                translate([0, 0, latch_h - latch_slot / 2])
                    rotate([90, 0, 0]) cylinder(d = latch_slot, h = 2 * skirt, center = true, $fn = 16);
            }
    }
    // latch hooks
    for (x = latch_x, sy = [-1, 1])
        along_wall(x - latch_w / 2, latch_w, sy) hook2d();
}

module plate() {
    difference() {
        union() {
            // floor and stiffening rim, the rim's inner edge chamfered
            difference() {
                hull() {
                    linear_extrude(edge_t - top_ch) outline(gap - plate_gap);
                    linear_extrude(edge_t) outline(gap - plate_gap - top_ch);
                }
                translate([0, 0, floor_t]) linear_extrude(edge_t)
                    outline(gap - plate_gap - rim_w);
            }
            // pads the standoffs sit on, with rings to locate them
            for (sx = [-1, 1], sy = [-1, 1]) {
                rh = len([for (q = ring_low) if (q == [sx, sy]) 1]) > 0 ? ring_low_h : ring_h;
                translate([sx * holes[0] / 2, sy * holes[1] / 2, floor_t - eps]) {
                    cylinder(d = ring_od, h = board_lift + eps);
                    translate([0, 0, board_lift]) difference() {
                        cylinder(d = ring_od, h = rh + eps);
                        translate([0, 0, -eps]) cylinder(d = ring_id, h = rh + 3 * eps);
                    }
                }
            }
        }
        // notches the latch hooks drop into
        for (x = latch_x, sy = [-1, 1])
            along_wall(x - latch_w / 2 - latch_clr - 0.2, latch_w + 2 * latch_clr + 0.4, sy) notch2d();
        // recess in the back for the Velcro patch
        translate([velcro_x, 0, -eps]) linear_extrude(velcro_d + eps)
            offset(r = 1.5) offset(delta = -1.5) square(velcro, center = true);
        // cut-out for the pin header
        translate([header[0], header[2], -eps])
            cube([header[1] - header[0], header[3] - header[2], edge_t + 1]);
        // cable opening at the USB end, open to the edge (the square end runs
        // out past it, so only the inner corners are rounded)
        translate([pl_x - usb_hole[0], usb_y - usb_hole[1] / 2, -eps])
            linear_extrude(edge_t + 1)
                offset(r = usb_hole_r) offset(delta = -usb_hole_r)
                    square([usb_hole[0] + usb_hole_r + 2, usb_hole[1]]);
    }
}

// rough stand-in for the board, for the previews
module board() {
    color("#111") translate([0, 0, glass_z - 1.1]) linear_extrude(1.1) outline(0);
    color("#9aa") translate([0, 0, glass_z - 5.9]) linear_extrude(4.8) offset(-0.8) outline(0);
    color("#1e4fa0") translate([-glass_w / 2 + 6.7, -glass_h / 2 + 1.3, glass_z - 7.5])
        cube([glass_w - 6.7 - 7.8, glass_h - 2.6, 1.6]);
    color("gold") for (sx = [-1, 1], sy = [-1, 1])
        translate([sx * holes[0] / 2, sy * holes[1] / 2, seat]) cylinder(d = 3.5, h = 4);
    color("#222") translate([header[0] + 1, header[2] + 1, seat - 2]) cube([header[1] - header[0] - 2, header[3] - header[2] - 2, 6]);
    color("#bbb") translate([glass_w / 2 - 14.3, usb_y - 4.5, usb_z - 1.6]) cube([7.5, 9, 3.2]);
}

if (part == "bezel") rotate([180, 0, 0]) translate([0, 0, -total]) bezel();   // print face down
else if (part == "plate") plate();                                             // print back down
else if (part == "exploded") {
    color("#d8d8d8") translate([0, 0, -30]) plate();
    translate([0, 0, -12]) board();
    color("#f9a11b", 0.9) bezel();
} else if (part == "section") {      // cut through a latch
    intersection() {
        translate([latch_x[1] - 0.5, -50, -1]) cube([1, 100, total + 2]);
        union() { color("#d8d8d8") plate(); board(); color("#f9a11b") bezel(); }
    }
} else {
    color("#d8d8d8") plate();
    board();
    color("#f9a11b") bezel();
}
