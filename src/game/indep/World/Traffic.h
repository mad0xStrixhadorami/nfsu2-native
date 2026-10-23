#pragma once
#include <cstdint>
#include <vector>

#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/TrafficNet.h"

class TrackWorld;

// Ambient traffic (ISSUES.md #130): cars that follow the free-roam lane links (TrafficNet) at a
// leisurely pace, brake for a car in front, and are recycled onto a link near the player once
// they fall too far behind. UG2's TrafficCarThink/TrafficEmitter logic is not ported.
class Traffic {
public:
    struct Driver {
        PlayerCar *car = nullptr;
        int link = -1;
        float t = 0; // metres along link
        float cruise = 14.0f;
        float steer = 0;
    };
    // cars: the models the traffic draws from (already assembled); count of drivers to run.
    void Init(const TrafficNet *net, std::vector<PlayerCar> *cars, unsigned seed);
    // others: cars to avoid (player and opponents), also used for spawn spacing.
    void Update(const TrackWorld &world, float dt, const std::vector<const PlayerCar *> &others);
    size_t Size() const { return m_drivers.size(); }

private:
    void Respawn(Driver &d, const TrackWorld &world, float px, float py, bool initial);
    unsigned Rand();
    const TrafficNet *m_net = nullptr;
    std::vector<Driver> m_drivers;
    unsigned m_seed = 1;
    float m_px = 0, m_py = 0, m_pz = 0;
    int m_lastDebug = -1;
    float m_clock = 0; // signal phase clock (seconds)
};
