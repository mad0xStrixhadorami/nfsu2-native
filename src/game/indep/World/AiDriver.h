#pragma once

class PlayerCar;
class RaceRoute;
class TrackWorld;

// Stand-in opponent driver (UG2's AI racer, which drives the road network's lanes, is not
// ported): steers a PlayerCar towards a point ahead on the race route, offset into its own lane,
// and picks a speed from the bend ahead.
class AiDriver {
public:
    // startS: route distance of the grid slot (same lap as the start line); <0 projects freely.
    void Init(PlayerCar *car, const RaceRoute *route, float lane, float skill, float startS = -1.0f);
    // go: the race is running. Returns the driver's unwrapped progress along the route (m).
    float Update(const TrackWorld &world, float dt, bool go);
    float Progress() const { return m_progress; }
    PlayerCar *Car() const { return m_car; }
    // Engine torque scale passed to the car (the drag gearbox drives the player's route-following
    // driver under NFSU2_AUTODRIVE / NFSU2_DRAG_AUTO; ISSUES #115).
    void SetPower(float p) { m_power = p; }

private:
    PlayerCar *m_car = nullptr;
    const RaceRoute *m_route = nullptr;
    float m_lane = 0;  // metres left of the route line
    float m_skill = 1; // top-speed scale
    float m_s = -1;    // last projected route distance
    float m_progress = 0;
    float m_steer = 0;
    float m_stuck = 0; // seconds without progress while trying to drive
    float m_power = 1; // engine torque scale (drag gearbox)
};
