#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/PoliceCar.h"
#include "game/indep/World/TrafficNet.h"

class TrackWorld;

// Free-roam police and the pursuit (ISSUES #318), in the manner of Most Wanted / Carbon / Unbound:
//   patrols cruise the lanes; one that sees the car speeding, or is rammed, starts a PURSUIT at heat 1;
//   the heat climbs with the time and with every ram, bringing more cars, harder ones and roadblocks;
//   the player gets away by staying out of sight (COOLDOWN fills, ESCAPED pays the bounty) or is BUSTED when
//   a cop pins the car down at a crawl (the BUSTED bar fills, the fine is taken).
// The cars are the imported Israeli police Skoda on the real UG2 vehicle physics (the record of a stock car with the Skoda's wheels
// patched in) steering along the free-roam lane network (TrafficNet): an A* route to the lane under the player, then a direct
// pursuit when close.
class Pursuit {
public:
    enum State { kIdle, kChase, kCooldown, kBusted, kEscaped };
    enum Mode { kOff, kPatrol, kPursue, kBlock };
    struct Unit {
        PlayerCar car;
        PlayerCar lod;         // the far model: the same car with a rough mesh, no physics
        size_t worldIdx = 0, worldLod = 0;
        Mode mode = kOff;
        int link = -1;
        float t = 0;
        std::vector<int> route; // lane links to the player
        float immobile = 0, repath = 0, stuck = 0, reverse = 0, steer = 0, ramCool = 0, sees = 0, age = 0, lost = 0;
        int goalLink = -2;
        bool siren = false;
    };
    // What the HUD shows.
    struct Hud {
        State state = kIdle;
        int heat = 0;
        int units = 0;       // cars in the chase
        float bounty = 0;
        float chaseTime = 0;
        float bust = 0;      // 0..1
        float cooldown = 0;  // 0..1
        float resultTimer = 0; // seconds since BUSTED / ESCAPED
        float fine = 0, reward = 0;
        float banner = 0;    // seconds a big banner has been up (PURSUIT / HEAT n)
        std::string bannerText, bannerSub;
        int rams = 0, copsHit = 0;
        float nearest = 1e9f; // distance to the nearest cop (siren volume, screen flash)
    };

    // mesh: the converted police car; record: a UG2 car record (the stock car's) whose wheels are patched here.
    bool Init(TrackWorld &world, const PoliceMesh &mesh, const PoliceMesh *lodMesh, const uint8_t *record, const TrafficNet *net, int maxUnits);
    // The far model of unit k (a PlayerCar whose Model() the world draws) and its world index.
    PlayerCar *LodCar(size_t k) { return k < m_units.size() ? &m_units[k].lod : nullptr; }
    size_t LodWorldIndex(size_t k) const { return k < m_units.size() ? m_units[k].worldLod : 0; }
    bool HasLod() const { return m_hasLod; }
    bool Ready() const { return !m_units.empty(); }
    std::vector<PlayerCar *> Cars();
    size_t Count() const { return m_units.size(); }
    // One frame: drives the cops, runs the pursuit.  dt = 0 while paused.
    void Update(TrackWorld &world, float dt, PlayerCar &player, bool racing);
    const Hud &GetHud() const { return m_hud; }
    // Dev / test starts: begin a pursuit now at `heat`.
    void Start(TrackWorld &world, PlayerCar &player, int heat);
    // Dev aid (NFSU2_PURSUIT_ROAD=<n>): puts the player on the n-th long lane with ground near the start, facing along it.
    bool DevPlacePlayer(TrackWorld &world, PlayerCar &player, int nth);
    // The result screen was dismissed: the cops go home (BUSTED also moves the player back; the caller applies fine / reward).
    void Dismiss();
    bool ResultPending() const { return m_state == kBusted || m_state == kEscaped; }
    State GetState() const { return m_state; }
    // The caller applies money once.
    bool TakeResult(bool &busted, float &amount);
    void SetEnabled(bool on) { m_enabled = on; }
    const PoliceMesh *Mesh() const { return m_mesh; }
    size_t FirstWorldIndex() const { return m_units.empty() ? 0 : m_units[0].worldIdx; }
    bool UnitSiren(size_t k) const { return k < m_units.size() && m_units[k].mode != kOff && m_units[k].siren; }
    // Distance to the nearest car with its siren on (-1 if none).
    float NearestSiren() const;

private:
    struct Node {
        std::vector<int> succ;
    };
    struct Seg {
        float x0, y0, x1, y1, length;
    };
    void BuildGraph();
    int NextOf(int link) const; // the lane after `link` (TrafficNet's own, else one of the lanes that start where it ends)
    int NearestLink(float x, float y, float maxDist, float yaw, bool useYaw) const;
    bool Route(int from, int goal, std::vector<int> &out) const;
    bool PlaceOnLink(Unit &u, TrackWorld &world, int link, float t, const PlayerCar &player);
    bool SpawnUnit(TrackWorld &world, const PlayerCar &player, Mode mode, float minD, float maxD, bool behind);
    void Recycle(Unit &u);
    void DriveUnit(Unit &u, TrackWorld &world, float dt, PlayerCar &player, float targetSpeed, float aimX, float aimY, float power);
    void UpdatePatrol(Unit &u, TrackWorld &world, float dt, PlayerCar &player);
    void UpdatePursuer(Unit &u, TrackWorld &world, float dt, PlayerCar &player);
    void UpdateBlock(Unit &u, TrackWorld &world, float dt, PlayerCar &player);
    void MakeRoadblock(TrackWorld &world, const PlayerCar &player);
    void Banner(const std::string &text, const std::string &sub);
    unsigned Rand();

    const PoliceMesh *m_mesh = nullptr;
    const TrafficNet *m_net = nullptr;
    std::vector<Unit> m_units;
    std::vector<Node> m_graph;
    std::vector<Seg> m_seg;
    State m_state = kIdle;
    Hud m_hud;
    bool m_enabled = true, m_hasLod = false;
    unsigned m_seed = 7;
    float m_clock = 0, m_spawnTimer = 0, m_roadblockTimer = 0, m_heatTimer = 0, m_unseen = 0, m_spawnPatrol = 0;
    float m_px = 0, m_py = 0, m_pvx = 0, m_pvy = 0, m_pz = 0, m_pyaw = 0, m_pspeed = 0;
    float m_resultAmount = 0;
    bool m_resultBusted = false, m_resultTaken = true, m_hadUnits = false, m_contact = false;
    int m_dbgSec = -1;
    float m_graceTimer = 0; // after a result, patrols stay away for a while
};
