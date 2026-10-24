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
    void SetMph(bool mph) { m_mph = mph; } // Options: speedometer units
    struct Config {
        int laps = 2;
        int type = 0;             // DAT_004f13b0: 0 circuit, 1 sprint, 2 drag, 3 drift, 4 URL, 10 street x
        float trackLength = 0;    // metres, race record +0x98
        bool hasLine = false;     // start/finish line from the race's TrackPosMarkers
        float lineX = 0, lineY = 0;
        float dirX = 1, dirY = 0; // direction of travel across the line
        const RaceRoute *route = nullptr; // when valid: progress, laps and wrong way along it
        float lineS = 0;                  // route distance of the start/finish line
        // Route distance of the player's grid slot, resolved against lineS. A race's Paths file can
        // hold several laps of the circuit, so the same tarmac projects onto several s values; the
        // free projection would pick the nearest lap, not the one the race starts on.
        float startS = -1.0f;             // <0: project the car when the race begins
        int racers = 1;                   // player + opponents
        // Options > Player / Gameplay (ISSUES #239): HUD elements and the minimap mode (0 off, 1 static, 2 rotate).
        bool showGauges = true, showPosition = true, showLapInfo = true, showLeaderboard = true;
        int raceMapMode = 2, exploreMapMode = 2;
        bool modernHud = true;    // gauges / position / lap / time are drawn by ModernUI instead of the FEng elements
        float introSeconds = 0;   // the cinematic pre-race camera (ISSUES #265): the countdown starts after it
    };
    enum Phase { kCountdown, kRacing, kFinished };

    void Begin(FEPackage *hud, const Config &config);
    int m_gaugeLines = 8500;    // the dial's full scale in rpm (7500 / 8500 / 10500)
    float m_intro = 0, m_introTotal = 0;
    bool m_gaugeSkinned = false; // the tachometer's custom textures are set (UpdateHud, ISSUES #260)
    // Advances the race; returns whether the player may drive. opponents: each opponent's race
    // distance (metres past the start line along the route) for the position display.
    bool Update(const PlayerCar &car, const std::vector<float> &opponents, float dt);
    // Race distance of a car at route distance s plus its accumulated progress.
    float RaceDistance(float startS, float progress) const;
    float LapLength() const;
    int Laps() const { return m_config.laps; }
    int Lap() const { return m_lap; }                 // the modern HUD (ISSUES #276)
    float LapTime() const { return m_lapTime; }
    int Position() const { return m_position; }
    int Racers() const { return m_racers; }
    int GaugeScale() const { return m_gaugeLines; }
    bool ModernHud() const { return m_config.modernHud; }
    float RaceLength() const { return m_config.laps * LapLength(); } // finish distance
    bool Running() const { return m_phase == kRacing; }
    float PlayerDistance() const { return RaceDistance(m_startS, m_progress); }
    Phase GetPhase() const { return m_phase; }
    // The pre-race cinematic: while it runs the HUD is hidden, the countdown waits and the car is held.
    bool InIntro() const { return m_phase == kCountdown && m_intro > 0.0f; }
    float IntroElapsed() const { return m_introTotal - m_intro; }
    float IntroTotal() const { return m_introTotal; }
    void SkipIntro();
    float TotalTime() const { return m_totalTime; }
    float BestLap() const { return m_bestLap; }

    // Drag race (type 2): a manual gearbox. UG2's transmission (0x0031d570 physics / 0x0025d4d8
    // race setup) is not ported, so this is a stand-in -- throttle free-revs the engine during the
    // countdown, the launch quality comes from the revs at green, and the player upshifts (Shift)
    // to stay off the limiter. DragUpdate returns the engine torque scale for PlayerCar::Update.
    bool IsDrag() const { return m_config.type == 2; }
    void Shift();
    float DragUpdate(float speed, float throttle, bool shift, float dt);
    int Gear() const { return m_gear; }
    float Rpm() const { return m_rpm; }
    bool NeedShift() const { return m_rpm > kAutoShiftRpm; } // test aid (NFSU2_DRAG_AUTO)
    bool ShiftLight() const { return m_rpm > kRedline * 0.92f; }

    // Drift race (type 3): a stand-in for UG2's DriftManager/DriftDriver scoring. The arcade car has
    // no lateral velocity, so the slip angle is modelled from the cornering rate (yaw rate * speed);
    // points accrue while "drifting", a chain multiplier grows with sustained drift, and the chain
    // is banked into the total after a short grace period without drifting.
    bool IsDrift() const { return m_config.type == 3; }
    void DriftUpdate(float speed, float yaw, float dt);
    float DriftPoints() const { return m_driftChain * m_driftMult; } // current chain value
    float DriftTotal() const { return m_driftTotal; }
    int DriftMultiplier() const { return m_driftMult; }
    float DriftAngle() const { return m_driftAngle; } // slip angle in degrees

    // URL (type 4): a lap knockout -- the last racer across the line each lap is eliminated, and the
    // race ends when one remains (or the laps run out). IsEliminated reports opponents knocked out;
    // TakeKnockout returns (once) the opponent index eliminated this frame and PlayerKnockedOut the
    // player's elimination.
    bool IsKnockout() const { return m_config.type == 4; }
    int ActiveRacers() const;
    bool IsEliminated(int opponentIndex) const {
        return opponentIndex >= 0 && opponentIndex < int(m_eliminated.size()) && m_eliminated[opponentIndex];
    }
    int TakeKnockout() { int k = m_knockoutEvent; m_knockoutEvent = -1; return k; }
    bool PlayerKnockedOut() const { return m_playerOut; }

    // Free run (type 9): roam the region with no race -- no countdown, laps, position or timer, and
    // the race HUD elements hidden (ISSUES #118).
    bool IsFreeRun() const { return m_config.type == 9; }

    // Outrun (type 8): a one-on-one freeway duel. UG2's version runs in free roam and is won by
    // opening a lead of the "getaway" distance; with the free-roam road network undecoded (ISSUES
    // #118) this is a stand-in on a normal route -- one rival, and the gap drives the get-away meter
    // (Getaway_Distance_Bar + Distance_Number/Distance_Text, strings 0x566fa8 "Ahead" / 0x566fa0
    // "Behind"). Reaching kOutrunEscape ahead escapes (win); being caught by the same margin loses.
    bool IsOutrun() const { return m_config.type == 8; }
    float OutrunLead() const { return m_outrunLead; } // metres the player is ahead of the rival
    bool OutrunCaught() const { return m_outrunCaught; }
    // The results screen replaces the HUD: drop the HUD pointer (main.cpp unloads the package).
    void HideHud();
    static constexpr float kOutrunEscape = 200.0f; // metre lead that ends the duel (stand-in)

private:
    bool m_mph = false;
    void SetupHud();
    void UpdateHud(const PlayerCar &car);
    void UpdateRoute(const PlayerCar &car, const std::vector<float> &opponents, float dt);
    void UpdateLine(const PlayerCar &car);
    void CompleteLap();
    void BuildStandings(float playerDist, const std::vector<float> &opponents);
    void Finish();

    static constexpr float kIdle = 1000.0f, kRedline = 7600.0f, kAutoShiftRpm = 7300.0f;
    static constexpr int kGears = 6;

    float m_nosBaseW = 0, m_nosBaseX = 0, m_nosBaseU = 0, m_nosBaseU1 = 1; // the N2O arc's shipped geometry
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
    int m_racers = 1;
    float m_wrongWay = 0;      // seconds spent driving against the route
    // Drag gearbox (see IsDrag).
    int m_gear = 1;
    float m_rpm = kIdle;
    float m_launchRpm = 0;     // engine speed when the countdown hit green
    float m_launchBoost = 1;   // torque multiplier from the launch quality
    float m_boostTime = 0;     // seconds the launch boost still applies
    // Drift scoring (see IsDrift).
    float m_driftAngle = 0;    // current slip angle (degrees)
    float m_driftChain = 0;    // chain points before the multiplier
    float m_driftTotal = 0;    // banked points
    int m_driftMult = 1;       // chain multiplier
    float m_driftTime = 0;     // seconds in the current chain
    float m_driftIdle = 0;     // seconds since the last scoring frame
    float m_prevYaw = 0;
    bool m_haveYaw = false;
    void BankDrift();
    // URL knockout (see IsKnockout).
    std::vector<bool> m_eliminated; // per opponent: knocked out
    std::vector<int> m_leaderboard;  // active racers, leader first (0 = player, i+1 = opponent i)
    int m_knockoutEvent = -1;        // opponent eliminated this frame (for the caller), else -1
    bool m_playerOut = false;
    // Outrun (see IsOutrun).
    float m_outrunLead = 0;          // metres the player leads the rival by (negative: behind)
    bool m_hudHidden = false;
    bool m_outrunCaught = false;     // the rival escaped instead of the player
};
