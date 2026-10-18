#pragma once
#include <cstdint>
#include <vector>

class FEPackage;
class PlayerCar;
class RaceRoute;

// Native stand-in for UG2's race flow and HUD class (not ported): a 3-2-1-GO countdown, lap
// timing and counting, a finish, and the HUD_SingleRace.fng elements that show them. Object
// names were recovered by hashing executable strings against the stripped package (ISSUES #106).
class RaceSession {
public:
    struct Config {
        int laps = 2;
        float trackLength = 0;    // metres, race record +0x98
        bool hasLine = false;     // start/finish line from the race's TrackPosMarkers
        float lineX = 0, lineY = 0;
        float dirX = 1, dirY = 0; // direction of travel across the line
        const RaceRoute *route = nullptr; // when valid: progress, laps and wrong way along it
        float lineS = 0;                  // route distance of the start/finish line
        int racers = 1;                   // player + opponents
    };
    enum Phase { kCountdown, kRacing, kFinished };

    void Begin(FEPackage *hud, const Config &config);
    // Advances the race; returns whether the player may drive. opponents: each opponent's race
    // distance (metres past the start line along the route) for the position display.
    bool Update(const PlayerCar &car, const std::vector<float> &opponents, float dt);
    // Race distance of a car at route distance s plus its accumulated progress.
    float RaceDistance(float startS, float progress) const;
    bool Running() const { return m_phase == kRacing; }
    float PlayerDistance() const { return RaceDistance(m_startS, m_progress); }
    Phase GetPhase() const { return m_phase; }
    float TotalTime() const { return m_totalTime; }
    float BestLap() const { return m_bestLap; }

private:
    void SetupHud();
    void UpdateHud(const PlayerCar &car);
    void UpdateRoute(const PlayerCar &car, const std::vector<float> &opponents, float dt);
    void UpdateLine(const PlayerCar &car);
    void CompleteLap();

    FEPackage *m_hud = nullptr;
    Config m_config;
    Phase m_phase = kCountdown;
    float m_countdown = 0;
    float m_goShown = 0;      // seconds the "GO!" text stays up
    int m_lap = 1;
    float m_lapTime = 0, m_totalTime = 0, m_bestLap = 0;
    float m_lapDistance = 0;  // odometer since the last line crossing
    float m_lastSide = 0;     // signed distance to the line on the previous update
    float m_prevX = 0, m_prevY = 0;
    bool m_havePrev = false;
    // Route progress.
    float m_routeS = -1;       // last projected route distance
    float m_startS = 0;        // route distance where the player started
    float m_progress = 0;      // unwrapped metres driven along the route
    int m_position = 1;
    float m_wrongWay = 0;      // seconds spent driving against the route
};
