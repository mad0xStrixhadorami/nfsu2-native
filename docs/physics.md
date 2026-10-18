# UG2 (PS2 PAL) vehicle physics map

Working notes for porting the real car physics (ISSUES.md #129). Addresses are SLES_527.25;
pseudocode is in `~/re/nfsu2_pal/reference/all/<addr>.c`. "rec" = car database record
(GLOBALB chunk 0x34600, 0x890 bytes); the physics parameter block `P` = rec + 0x118 (tuned copy).

## Integration

* `0x00317428` **Vehicle::Update(dt)** (vtable 0x0047ff88 slot 2): pre-steps 0x316fb0, 0x317748,
  0x317ac0, 0x317d68(dt), 0x319120; then `n = int(dt / DAT_00568b70) + 1` equal substeps
  (`DAT_00568c98` = substep length) of `0x0030a688`; accumulates rigid-body force/torque
  (+0x130/+0x140) into +0x100/+0x110 and averages them; then 0x318968(dt) and 0x329188(dt).
* `0x0030a688` **Integrator::Step(h)**: explicit Euler over a linked list of integrables
  (`0x0030a630` adds one): `0x0030a7c8` gathers every object's derivative (vtable +0x3c) into one
  array, then `state[i] += h * deriv[i]` for each object's state (size = vtable +0x14, pointer =
  vtable +0x1c), then the owner's post-step (vtable +0x14 of the integrator's owner).
* Vtable layout (GCC 2.x): 8-byte entries {s16 this-adjust, pad, fn}; slot n at +8n.

## Objects (constructed by `0x00316900` Vehicle ctor, `P` stored at vehicle +0x54)

| vehicle | object | ctor | vtable | params |
|---|---|---|---|---|
| +0x20 | RigidBody | 0x321fd8 | ? | P+0x110 (rec+0x228: mass, length, width, height, ?) |
| +0x24 | ? (steering pair) | inline | 0x47fa70 | P |
| +0x28..+0x34 | 4 x Suspension | 0x324e90 | 0x47fa20 | P+0x170 front / +0x190 rear (rec+0x288/+0x2a8) |
| +0x38..+0x44 | 4 x Tyre | 0x323410 | ? | wheel P+0x10+0x30i (rec+0x128), axle P+0xd0/+0xf0 (rec+0x1e8/+0x208), grip P+0x270 (rec+0x388) |
| +0x48 | Engine | 0x30bf90 | 0x47ff18 | P+0x1f0 (rec+0x308: idle/redline/max rpm, torque curve) |
| +0x4c | Drivetrain | 0x30c950 | 0x47fec8 | P+0x1b0 (rec+0x2c8: efficiency, final drive, gears) |
| +0x50 | Aero | inline | - | P+0x260 (rec+0x378) |

Suspensions are paired for anti-roll: 0<->1 (front), 2<->3 (rear) via +0x18.

## Vehicle tick (derivative) `0x00318208`

1. RigidBody pre (vtable +0x34), 0x302620 (aero/steer), Suspension pre 0x325338 x4
2. `0x003183a0` tyres: drivetrain 0x30da30; per tyre 0x323eb0 (slip/forces); 5 iterations of:
   sum tyre forces 0x3248c8 -> body acceleration (mass P+0x164 of RigidBody), yaw from
   +0x90/+0x94 lever arms, apply per-tyre 0x324408; drivetrain 0x30db88. Then per tyre
   `0x00324460` friction circle and drivetrain 0x30dce0.
3. Suspension post 0x325378 x4, 0x318ee8, 0x367a58 (if +0x5c), 0x318fc0 (some modes),
   0x3185e8, 0x3195a8 (if +0x84).

## Suspension `0x00324ff8` (state +0x1c compression x, +0x20 rate v)

params p = rec+0x288 (front) / +0x2a8 (rear), floats:
`p0` progressive, `p1` k, `p2` damping when v > 0, `p3` when v <= 0, `p4` anti-roll,
`p5` max travel, `p6` min travel, `p7` ?.
x = ground probe 0x323b38 + tyre (+0x14)+0x10, clamped to [p6, p5] (grounded flag +0x80 = 0
when below p6). v = (x - x_prev) / h.
`F = (+0x88) + x*p1*(1 + p0*max(x,0)) + v*(p2|p3) + p4*(x - x_other)`, F < 0 -> 0, stored +0x24.
Also writes the wheel frame (steer angle from tyre +0x24 x (+0x174) x DAT_005638e8).

## Tyre friction circle `0x00324460`

limit = DAT_00568c68 * (+0x188) * (+0x164) * (+0x180) * surface[+0x114].mu (table 0x0052f954,
0x98 bytes/surface) * (+0x18)->0xc * (+0x184) [* 0x323e10 if ...]; the force (+0x124 long,
+0x128 lat) is scaled onto the circle; +0x170 = |F| / limit (saturation, 1 when not grounded).
DAT_00568c88 selects the force-frame mode (1..4).

## RigidBody (ctor `0x00321fd8`, vtable 0x00480610)

State (0x20 floats from +0x20): position +0x20, orientation 4x4 +0x30..+0x6f (rows = body axes),
linear momentum +0x70, angular momentum +0x80, +0x90 rest/sleep scalar. Params P+0x110 = rec+0x228:
mass (t) +0, dimensions +4..+0xc, inertia 4x4 +0x10 (diagonal, z scaled by rec+0x278); `0x00322348`
caches m (+0x164), 1/m (+0x168), inverse inertia (+0x170, `0x00152830` = 4x4 affine inverse).
* derivative `0x00322c98`: dpos = v (+0xa0); dR = `0x00322b38`(omega +0xc0, R); dP = F (+0x110);
  dL = torque (+0x120) - L * curve(|omega|, 0x51df38) when +0x154; d(+0x90) = 0.1 if +0x90 <
  curve(max(|v|/4, |omega|/2), 0x51df70) else -10; when +0x150: extra damping dL -= 2*rest*L,
  dP -= 8*rest*P; yaw torque clamp +0x200. A low-speed "parking" grip term uses rec values via
  +0x204/+0x208 (1.9, 1.03, 0.3, 1/3, 2.236).
* post-step `0x00323030`: v = P/m (|v| capped at 200 m/s); omega = R Iinv R^T L (|omega| <= 250);
  keeps previous F/T in +0x130/+0x140, clears F/T, adds gravity F = (0,0,-9.8 m) via `0x00322540`
  (apply force at a point). Orientation is re-orthonormalised each substep (`0x001e0e40`).
* Constants: g = 9.8 (gp-0xbb8), substep 0.0044 s (DAT_00568b70).

## Tyre (ctor `0x00323410`, vtable 0x004805c0)

+0x14 wheel geometry (rec+0x128 + 0x30i: x, y, z, +0x10 radius, +0x14 width), +0x18 axle params
(rec+0x1e8 front / +0x208 rear: [0] slip-angle scale 12, [1] load normaliser, [2] lateral scale,
[3] friction-circle mu, [4] longitudinal mu), +0x1c brakes (rec+0x388: [1] brake, [2] handbrake),
+0x20 vehicle. +0x28 wheel spin (rad/s), +0x108 wheel torque, +0x164 load, +0x178 brake input,
+0x17c handbrake input, +0x180/+0x184/+0x188/+0x190/+0x194 grip multipliers (1).
* `0x00323eb0` slip/forces: brake torques 4*brake*[1], 10*handbrake*[2] oppose spin; ground
  velocity in wheel frame (vx, vy); slip speed = spin*radius - vx (+0x118); lock test
  `0x00323d78`; if sliding (|slip| > 0.5 or lateral beyond [0] or locked): force along the slip
  vector with magnitude load * mu (mu = [4] * multipliers * surface[+0x114]), else rolling:
  long = torque / radius, lateral = `0x003242b0`(load, |slip angle|): curves 0x51e068 + 0x18*band
  (7 bands of lateral force vs normalised load 0..10, band = |angle deg| * 12/[0] * 0.5,
  interpolated) * [2] * 2.5.
* `0x00324408`: corrects the longitudinal force so wheel and body velocities agree.
* Curves are built at run time by static initializer `0x00328738` (replay with
  `tools/mips_init_run.py ... 328738`; curve = {n, xmin, xmax, (n-1)/(xmax-xmin), data*},
  lookup `0x002edf48` clamps and lerps).

## Not yet read

RigidBody (0x321xxx-0x322xxx), Tyre slip 0x323eb0, Drivetrain 0x30c950/0x30d*, Engine 0x30bf90,
Aero 0x302620, pre/post steps 0x316fb0/0x317748/0x317ac0/0x317d68/0x319120/0x318968/0x329188,
collision (0x334110, 0x336180, 0x337888), constants (DAT_0056xxxx, gp = 0x5695f0).
