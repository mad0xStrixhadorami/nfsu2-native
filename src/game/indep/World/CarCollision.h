#pragma once
class PlayerCar;

// Car-to-car contacts for cars with UG2 rigid bodies (ISSUES #219).  A stand-in for the original's
// contact solver (CarCollisionBody, 0x00334110 and the SIMD solver, not decoded): the two cars'
// footprints (rectangles from the wheel hardpoints) are tested with the separating-axis method, and
// an overlap is separated and answered with an impulse (restitution and Coulomb friction) at the
// deepest corner.  Returns the closing speed along the contact normal (m/s, 0 = no contact), or a
// negative value when either car has no rigid body (the caller keeps its arcade fallback).
float ResolveCarContact(PlayerCar &a, PlayerCar &b);
