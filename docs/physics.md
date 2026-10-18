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

## Not yet read

RigidBody (0x321xxx-0x322xxx), Tyre slip 0x323eb0, Drivetrain 0x30c950/0x30d*, Engine 0x30bf90,
Aero 0x302620, pre/post steps 0x316fb0/0x317748/0x317ac0/0x317d68/0x319120/0x318968/0x329188,
collision (0x334110, 0x336180, 0x337888), constants (DAT_0056xxxx, gp = 0x5695f0).
