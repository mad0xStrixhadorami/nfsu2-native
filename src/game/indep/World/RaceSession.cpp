#include "game/indep/World/RaceSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Speed/Indep/Src/FEng/FEGroup.h"
#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "game/indep/Frontend/FENative.h"
#include "Speed/Indep/Src/FEng/FEImage.h"
#include "game/indep/World/CarMaterial.h"
#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/RaceRoute.h"

namespace {
// HUD_SingleRace.fng objects (stripped package: names from bStringHash matches).
constexpr uint32_t kGaugeCluster = 0x5164d4ea;
constexpr uint32_t kGaugeBacking = 0x02ddf58c;   // "Group" behind the gauges
constexpr uint32_t kTurboGroup = 0xc5d551b7;     // TURBO_GROUP
constexpr uint32_t kNosLine = 0x11495cd6, kNosFill = 0x11461255, kNosGameBreaker = 0x39e180c4;
constexpr uint32_t kShiftLight = 0x25cf2f74;
constexpr uint32_t kSpeed = 0xc1d5ff50;          // 3rdPersonSpeed
constexpr uint32_t kGear = 0xb84589be;           // 3rdPersonGear
constexpr uint32_t kNeedle = 0xf0250dac;         // 3rdPersonNeedle
constexpr uint32_t kMinimapMask = 0x40cdc515, kMinimapNorth = 0xf2a54430, kTrackMap = 0x90fe80f3;
constexpr uint32_t kPosition = 0xe8434eb4, kPositionBacking = 0x411301e3;
constexpr uint32_t kPositionNumber = 0x9c183bf8, kPositionNth = 0x44f24239, kNumberRacers = 0x3bbd6268;
constexpr uint32_t kLapGroup = 0x70f90bd4, kLapNumber = 0x57f0e3a5, kNumberLaps = 0xf2685238;
constexpr uint32_t kLapTimeGroup = 0x54be67bc, kCurrentLapTime = 0x30d9802e;
constexpr uint32_t kCountdownText = 0x2da220aa;  // 321_GO
constexpr uint32_t kRaceOverMessage = 0xa7806ddb, kRaceOverBacking = 0x4e4566e5;
constexpr uint32_t kWrongWay = 0xc82fa200; // WRONGWAYIMAGE
// Drift elements, in HUD_Drift.fng (GLOBAL\INGAMEDRIFT.BUN): the in-game HUD keeps the gauge
// cluster and adds the drift gauge (angle) and score (ISSUES #116).
constexpr uint32_t kStylePointsGroup = 0x79a62cba, kDriftBannerGroup = 0x1ac732cd;
constexpr uint32_t kDriftTotalGroup = 0xf90b0ac6, kSpeedTrapGroup = 0x7be44faa;
constexpr uint32_t kAngleGroup = 0x17050861;
constexpr uint32_t kDriftPoint = 0x39972fa3;    // SP_Drift_Point
constexpr uint32_t kDriftTotal = 0x39dfbebd;    // SP_DRIFT_TOTAL
constexpr uint32_t kDriftMult = 0xe5d37182;     // drift_multiplier_text
constexpr uint32_t kDriftSpeedData = 0x6a4beef0;// SpeedTrap_Speed_DATA (the gain-hint number)
constexpr uint32_t kDriftBestTime = 0x06e9a0fe; // BestLapTimeText
constexpr uint32_t kDriftAngleDeg = 0x30787b84; // Angle_degrees
constexpr uint32_t kDriftAngleData = 0xb5c6103b;// Angle_Indicator_DATA
constexpr uint32_t kCommentData = 0xd555b3ab;   // Comment_DATA
// Standings board (LeaderBoardGroup) used by URL races; the row children are anonymous, addressed by
// their ordinal hash. Position number strings and the name strings of the four rows.
constexpr uint32_t kLeaderBoardGroup = 0x5ed609a1;
constexpr uint32_t kBoardPos[4] = {0x4183e0a1, 0x4183e0a2, 0x4183e0a3, 0x4183e0a4};
constexpr uint32_t kBoardName[4] = {0x2af89fd7, 0x2af89fd8, 0x2af89fd9, 0x2af89fda};
constexpr uint32_t kBoardPlate[4] = {0x4fddd64c, 0x4fddd64d, 0x4fddd64e, 0x4fddd64f};
constexpr uint32_t kBoardIcon[4] = {0xfa3d7b71, 0xfa3d7b72, 0xfa3d7b73, 0xfa3d7b74};
// Out-run (type 8) get-away meter, in HUD_SingleRace.fng: the Out_Run_Message banner and the
// Getaway_Distance group (a backing, the guide lines, the bar, and the distance number/units). The
// bar is a 256x16 image whose Size scales it, so the fraction of the lead fills it from the left.
constexpr uint32_t kOutRunMessage = 0x407919af;
constexpr uint32_t kGetawayGroup = 0x8ca76279;
constexpr uint32_t kGetawayBacking = 0x238f4b29, kGetawayLines = 0xc81a81f5, kGetawayBar = 0xacbaf9ef;
constexpr uint32_t kDistanceNumber = 0x92cdc512, kDistanceText = 0xbcea10ce;
constexpr float kGetawayBarWidth = 256.0f;

std::string FormatTime(float seconds)
{
    int minutes = int(seconds / 60.0f);
    char text[32];
    snprintf(text, sizeof(text), "%d:%05.2f", minutes, seconds - minutes * 60.0f);
    return text;
}

// Drag gearbox curves (stand-in, see RaceSession::IsDrag): the wheel speed at the rev limiter in
// each gear (within the arcade car's ~45 m/s realistic top speed, so a clean run uses all six),
// and an engine torque curve with a power band that rewards shifting at the right time.
const float kGearTop[] = {11.0f, 20.0f, 30.0f, 40.0f, 49.0f, 58.0f};
// Revs a held throttle settles at during the countdown / at low speed: inside the perfect-launch
// window (digital buttons cannot hold a partial throttle, so a full hold is the "perfect" launch).
constexpr float kLaunchRev = 6900.0f;
float EngineTorque(float rpm)
{
    constexpr float kIdle = 1000.0f, kRedline = 7600.0f;
    if (rpm > kRedline)
        return 0.0f;
    if (rpm < kIdle)
        return 0.5f;
    float t = (rpm - kIdle) / (kRedline - kIdle);
    if (t < 0.35f)
        return 0.55f + 0.6f * (t / 0.35f);
    if (t < 0.85f)
        return 1.15f;
    return 1.15f - 0.3f * ((t - 0.85f) / 0.15f);
}

// Drift scoring (stand-in, see RaceSession::IsDrift): a drift counts above this speed and slip
// angle, the multiplier steps every kDriftChainTime seconds of continuous drift, and the chain is
// banked after kDriftGrace seconds without scoring.
constexpr float kDriftMinSpeed = 6.0f, kDriftMinAngle = 12.0f, kDriftChainTime = 1.5f, kDriftGrace = 1.0f;
constexpr int kDriftMaxMult = 10;
} // namespace

void RaceSession::Begin(FEPackage *hud, const Config &config)
{
    m_hud = hud;
    m_nosBaseW = 0.0f;
    m_config = config;
    if (m_config.route && m_config.route->Valid() && !m_config.route->Closed())
        m_config.laps = 1; // sprint
    // Free run (type 9): drive at once, no countdown (ISSUES #118).
    m_phase = IsFreeRun() ? kRacing : kCountdown;
    m_countdown = IsFreeRun() ? 0.0f : 3.0f;
    m_goShown = 0;
    m_lap = 1;
    m_lapTime = m_totalTime = m_bestLap = m_lapDistance = 0;
    m_havePrev = false;
    m_routeS = -1;
    m_progress = 0;
    m_position = 1;
    m_wrongWay = 0;
    m_gear = 1;
    m_rpm = kIdle;
    m_launchRpm = 0;
    m_launchBoost = 1;
    m_boostTime = 0;
    m_driftAngle = m_driftChain = m_driftTotal = m_driftTime = m_driftIdle = 0;
    m_driftMult = 1;
    m_haveYaw = false;
    m_eliminated.assign(std::max(0, m_config.racers - 1), false);
    m_leaderboard.clear();
    m_knockoutEvent = -1;
    m_playerOut = false;
    m_outrunLead = 0;
    m_outrunCaught = false;
    m_hudHidden = false;
    if (IsKnockout())
        m_config.laps = std::max(1, m_config.racers - 1); // one elimination per lap, down to one
    SetupHud();
    printf("[Race] begin: %d laps, type %d, track %.0f m, line %s\n", m_config.laps, config.type,
           config.trackLength, config.hasLine ? "from markers" : "none");
}

void RaceSession::SetupHud()
{
    if (!m_hud)
        return;
    // Show only the single-race elements; the package also holds online, career, drag, drift and
    // world-map widgets that UG2's HUD class hides per mode.
    static const uint32_t kShown[] = {kGaugeCluster, kGaugeBacking, kMinimapMask, kMinimapNorth, kTrackMap,
                                      kPosition, kPositionBacking, kLapGroup, kLapTimeGroup, kCountdownText};
    // Drift races (HUD_Drift.fng) also show the drift gauge and the style-point groups.
    static const uint32_t kDriftShown[] = {kStylePointsGroup, kDriftBannerGroup, kDriftTotalGroup,
                                           kSpeedTrapGroup, kAngleGroup};
    const bool sprint = m_config.route && m_config.route->Valid() && !m_config.route->Closed();
    for (FEObject *o = m_hud->GetFirstObject(); o; o = o->GetNext()) {
        bool shown = std::find(std::begin(kShown), std::end(kShown), o->NameHash) != std::end(kShown);
        if (IsDrift() && std::find(std::begin(kDriftShown), std::end(kDriftShown), o->NameHash) != std::end(kDriftShown))
            shown = true;
        // URL races show the standings board (the row children are set in UpdateHud).
        if (IsKnockout() && (o->NameHash == kLeaderBoardGroup ||
                             std::find(std::begin(kBoardPos), std::end(kBoardPos), o->NameHash) != std::end(kBoardPos) ||
                             std::find(std::begin(kBoardName), std::end(kBoardName), o->NameHash) != std::end(kBoardName) ||
                             std::find(std::begin(kBoardPlate), std::end(kBoardPlate), o->NameHash) != std::end(kBoardPlate) ||
                             std::find(std::begin(kBoardIcon), std::end(kBoardIcon), o->NameHash) != std::end(kBoardIcon)))
            shown = true;
        // Out-run shows the get-away meter and its banner.
        if (IsOutrun() && (o->NameHash == kGetawayGroup || o->NameHash == kOutRunMessage))
            shown = true;
        FENativeInterface::SetObjectVisible(o, shown && !(sprint && o->NameHash == kLapGroup));
    }
    for (uint32_t h : {kTurboGroup, kNosLine, kNosFill, kNosGameBreaker, kShiftLight})
        FENativeInterface::SetVisible(m_hud, h, false);
    // The get-away meter's children (the group is toggled in the loop above).
    for (uint32_t h : {kGetawayBacking, kGetawayLines, kGetawayBar, kDistanceNumber, kDistanceText})
        FENativeInterface::SetVisible(m_hud, h, IsOutrun());
    // The get-away group is a transparent container: the engine skips a group whose colour alpha is
    // zero (FEngine::RenderGroup), so give it an opaque colour (and the children one) to draw it.
    for (uint32_t h : {kGetawayGroup, kGetawayBacking, kGetawayLines, kGetawayBar, kDistanceNumber, kDistanceText})
        if (FEObject *o = m_hud->FindObjectByHash(h))
            if (FEObjData *d = o->GetObjData())
                d->Col.a = IsOutrun() ? 255 : 0;
    // Player options hide whole HUD groups.
    auto hide = [this](std::initializer_list<uint32_t> hashes) {
        for (uint32_t h : hashes)
            FENativeInterface::SetVisible(m_hud, h, false);
    };
    if (!m_config.showGauges)
        hide({kGaugeCluster, kGaugeBacking});
    if (!m_config.showPosition)
        hide({kPosition, kPositionBacking});
    if (!m_config.showLapInfo)
        hide({kLapGroup, kLapTimeGroup});
    if (!m_config.showLeaderboard)
        hide({kLeaderBoardGroup});
    if ((IsFreeRun() ? m_config.exploreMapMode : m_config.raceMapMode) == 0)
        hide({kMinimapMask, kMinimapNorth, kTrackMap});
    // Free run keeps only the gauges and minimap (ISSUES #118).
    if (IsFreeRun())
        for (uint32_t h : {kPosition, kPositionBacking, kLapGroup, kLapTimeGroup, kCountdownText,
                           kRaceOverMessage, kRaceOverBacking, kWrongWay})
            FENativeInterface::SetVisible(m_hud, h, false);
    char text[16];
    snprintf(text, sizeof(text), "/%d", m_config.laps);
    FENativeInterface::SetString(m_hud, kNumberLaps, text);
    snprintf(text, sizeof(text), "/%d", m_config.racers);
    FENativeInterface::SetString(m_hud, kNumberRacers, text);
}

// A circuit lap is the whole loop; a sprint (open route) is one "lap" of the race record's length
// from the start line (the route itself runs on past the finish).
float RaceSession::LapLength() const
{
    const RaceRoute *r = m_config.route;
    if (!r)
        return m_config.trackLength;
    if (r->Closed())
        return r->Length();
    return m_config.trackLength > 0 ? m_config.trackLength : r->Length() - m_config.lineS;
}

float RaceSession::RaceDistance(float startS, float progress) const
{
    const RaceRoute *r = m_config.route;
    return r ? r->Delta(m_config.lineS, startS) + progress : progress;
}

void RaceSession::Finish()
{
    if (IsDrift() && m_driftChain > 0)
        BankDrift();
    m_phase = kFinished;
    printf("[Race] finished %d of %d in %s (best lap %s)\n", m_position, m_config.racers,
           FormatTime(m_totalTime).c_str(), FormatTime(m_bestLap).c_str());
    if (IsKnockout() && m_playerOut)
        printf("[Race] knocked out on lap %d\n", m_lap - 1);
    if (IsDrift())
        printf("[Drift] final score %d\n", int(m_driftTotal));
    if (IsOutrun())
        printf("[Race] outrun: lead %.0f m, %s\n", m_outrunLead,
               m_outrunCaught ? "caught" : m_position == 1 ? "ahead" : "behind");
}

// Active racers: the player (unless knocked out) plus every opponent still in.
int RaceSession::ActiveRacers() const
{
    int n = m_playerOut ? 0 : 1;
    for (bool out : m_eliminated)
        n += out ? 0 : 1;
    return n;
}

// Standings (leader first) from the player's race distance and the opponents'; knocked-out
// opponents are left out so they drop off the board and out of the position count.
void RaceSession::BuildStandings(float playerDist, const std::vector<float> &opponents)
{
    m_leaderboard.clear();
    if (!m_playerOut)
        m_leaderboard.push_back(0);
    for (size_t i = 0; i < opponents.size(); ++i)
        if (!IsEliminated(int(i)))
            m_leaderboard.push_back(int(i) + 1);
    std::sort(m_leaderboard.begin(), m_leaderboard.end(), [&](int a, int b) {
        float da = a == 0 ? playerDist : opponents[size_t(a - 1)];
        float db = b == 0 ? playerDist : opponents[size_t(b - 1)];
        return da > db;
    });
}

void RaceSession::CompleteLap()
{
    printf("[Race] lap %d: %s\n", m_lap, FormatTime(m_lapTime).c_str());
    m_bestLap = m_bestLap > 0 ? std::min(m_bestLap, m_lapTime) : m_lapTime;
    m_lapDistance = 0;
    m_lapTime = 0;
    ++m_lap;
    const bool finalLap = m_lap > m_config.laps;
    // URL: knock out the last racer across the line each lap; the race ends when one remains.
    if (IsKnockout() && !m_playerOut && !m_leaderboard.empty()) {
        int last = m_leaderboard.back();
        if (last == 0) {
            m_playerOut = true;
            m_position = std::max(1, ActiveRacers() + 1);
            Finish();
            return;
        }
        m_eliminated[size_t(last - 1)] = true;
        m_knockoutEvent = last - 1;
        printf("[Race] car %d knocked out (last on lap %d)\n", last, m_lap - 1);
        if (ActiveRacers() <= 1) {
            m_position = 1;
            Finish();
            return;
        }
    }
    if (finalLap)
        Finish();
}

void RaceSession::Shift()
{
    if (m_phase == kRacing && m_gear < kGears)
        ++m_gear;
}

// Drag transmission: free-rev during the countdown, then a wheel-speed-driven rev in the current
// gear; the returned torque scale feeds PlayerCar::Update, so not upshifting pins the car to the
// limiter. The launch boost comes from the revs held at green (a "perfect launch").
float RaceSession::DragUpdate(float speed, float throttle, bool shift, float dt)
{
    if (m_phase == kCountdown) {
        float target = kIdle + std::clamp(throttle, 0.0f, 1.0f) * (kLaunchRev - kIdle);
        m_rpm += (target - m_rpm) * std::min(1.0f, 6.0f * dt);
        m_launchRpm = m_rpm;
        return 0.0f;
    }
    if (shift)
        Shift();
    float ratio = std::clamp(speed / kGearTop[std::clamp(m_gear - 1, 0, kGears - 1)], 0.0f, 1.1f);
    float engaged = kIdle + ratio * (kRedline - kIdle);
    // Clutch slip: below ~5 m/s the engine follows the throttle rather than the wheels.
    float clutch = std::clamp(1.0f - speed / 5.0f, 0.0f, 1.0f);
    float free = kIdle + std::clamp(throttle, 0.0f, 1.0f) * (kLaunchRev - kIdle);
    m_rpm = engaged * (1.0f - clutch) + free * clutch;
    float boost = 1.0f;
    if (m_boostTime > 0) {
        boost = m_launchBoost;
        m_boostTime = std::max(0.0f, m_boostTime - dt);
    }
    return EngineTorque(m_rpm) * boost;
}

void RaceSession::BankDrift()
{
    m_driftTotal += m_driftChain * m_driftMult;
    m_driftChain = 0;
    m_driftMult = 1;
    m_driftTime = 0;
}

// Drift scoring: the slip angle comes from the cornering rate (the arcade car does not slide), so a
// hard corner at speed reads as a drift; points accrue with speed * angle and a chain multiplier
// grows with sustained drift.
void RaceSession::DriftUpdate(float speed, float yaw, float dt)
{
    if (m_phase != kRacing) {
        m_driftAngle = 0;
        m_haveYaw = false;
        return;
    }
    const float yawRate = m_haveYaw && dt > 0 ? std::remainder(yaw - m_prevYaw, 6.2831853f) / dt : 0.0f;
    m_prevYaw = yaw;
    m_haveYaw = true;
    // Lateral acceleration yawRate * v against a ~1.2 g reference, as a slip angle in degrees.
    m_driftAngle = std::min(60.0f, std::atan2(std::fabs(yawRate) * std::fabs(speed), 12.0f) * 57.2958f);
    if (std::fabs(speed) > kDriftMinSpeed && m_driftAngle > kDriftMinAngle) {
        m_driftChain += std::fabs(speed) * m_driftAngle * 0.05f * dt;
        m_driftTime += dt;
        m_driftIdle = 0;
        m_driftMult = std::min(kDriftMaxMult, 1 + int(m_driftTime / kDriftChainTime));
    } else if ((m_driftIdle += dt) > kDriftGrace && m_driftChain > 0) {
        BankDrift();
    }
}

// Progress along the decoded route: laps, finish, position and wrong way.
void RaceSession::UpdateRoute(const PlayerCar &car, const std::vector<float> &opponents, float dt)
{
    const RaceRoute &route = *m_config.route;
    if (m_routeS < 0) {
        // The grid sits on the same lap as the line (the startS hint); free projection would jump to
        // a later lap of the same tarmac on multi-lap Paths files (ISSUES #166).
        m_routeS = m_startS = m_config.startS >= 0
                                  ? m_config.startS
                                  : route.Project(car.X(), car.Y(), m_config.hasLine ? m_config.lineS : -1.0f);
        m_progress = 0;
    }
    float s = route.Project(car.X(), car.Y(), m_routeS);
    m_progress += route.Delta(m_routeS, s);
    m_routeS = s;
    const float dist = RaceDistance(m_startS, m_progress);
    // Standings (leader first): the player's position and, for URL, the knockout order.
    if (m_phase == kRacing)
        BuildStandings(dist, opponents);
    if (m_phase == kRacing && dist >= m_lap * LapLength())
        CompleteLap();
    // Position: opponents further along the route are ahead (knocked-out ones are out of the count).
    if (m_phase == kRacing) {
        auto it = std::find(m_leaderboard.begin(), m_leaderboard.end(), 0);
        m_position = it == m_leaderboard.end() ? ActiveRacers() + 1 : int(it - m_leaderboard.begin()) + 1;
    }
    // Out-run (type 8): the gap to the rival drives the get-away meter; opening the escape distance
    // wins the duel, being caught by it loses.
    if (m_phase == kRacing && IsOutrun() && !opponents.empty()) {
        float rival = opponents[0];
        for (float o : opponents)
            rival = std::max(rival, o);
        m_outrunLead = dist - rival;
        if (m_outrunLead >= kOutrunEscape) {
            m_position = 1;
            Finish();
            return;
        }
        if (m_outrunLead <= -kOutrunEscape) {
            m_outrunCaught = true;
            m_position = m_config.racers;
            Finish();
            return;
        }
    }
    // Wrong way: moving against the route direction for a second.
    float x, y, dx, dy;
    route.Sample(s, x, y, dx, dy);
    float along = std::cos(car.Yaw()) * dx + std::sin(car.Yaw()) * dy;
    bool against = car.Speed() > 3.0f ? along < -0.3f : car.Speed() < -3.0f ? along > 0.3f : false;
    m_wrongWay = against ? m_wrongWay + dt : 0.0f;
}

// Fallback without a route: crossing the start/finish marker after most of a lap.
void RaceSession::UpdateLine(const PlayerCar &car)
{
    if (m_havePrev)
        m_lapDistance += std::hypot(car.X() - m_prevX, car.Y() - m_prevY);
    if (!m_config.hasLine)
        return;
    float rx = car.X() - m_config.lineX, ry = car.Y() - m_config.lineY;
    float side = rx * m_config.dirX + ry * m_config.dirY;
    float across = std::fabs(-rx * m_config.dirY + ry * m_config.dirX);
    if (m_phase == kRacing && m_havePrev && m_lastSide < 0 && side >= 0 && across < 25.0f &&
        m_lapDistance > 0.6f * m_config.trackLength)
        CompleteLap();
    m_lastSide = side;
}

bool RaceSession::Update(const PlayerCar &car, const std::vector<float> &opponents, float dt)
{
    if (m_phase == kCountdown) {
        m_countdown -= dt;
        if (m_countdown <= 0) {
            m_phase = kRacing;
            m_goShown = 1.0f;
            // Launch quality from the revs held at green: a window around 5500-7000 rpm is good.
            m_launchBoost = m_launchRpm >= 5200.0f && m_launchRpm <= 7000.0f ? 1.2f : m_launchRpm > 4200.0f ? 1.0f : 0.75f;
            m_boostTime = 1.5f;
        }
    } else if (m_phase == kRacing) {
        m_lapTime += dt;
        m_totalTime += dt;
        m_goShown = std::max(0.0f, m_goShown - dt);
    }
    if (m_config.route && m_config.route->Valid())
        UpdateRoute(car, opponents, dt);
    else
        UpdateLine(car);
    m_prevX = car.X(), m_prevY = car.Y();
    m_havePrev = true;
    UpdateHud(car);
    return m_phase == kRacing;
}

void RaceSession::HideHud()
{
    // The caller unloads the HUD package; forget it so no later update touches it.
    m_hudHidden = true;
    m_hud = nullptr;
}

void RaceSession::UpdateHud(const PlayerCar &car)
{
    if (!m_hud || m_hudHidden)
        return;
    // Countdown and GO (321_GO).
    char text[32];
    if (m_phase == kCountdown) {
        snprintf(text, sizeof(text), "%d", int(std::ceil(m_countdown)));
        FENativeInterface::SetString(m_hud, kCountdownText, text);
        FENativeInterface::SetVisible(m_hud, kCountdownText, true);
    } else if (m_goShown > 0) {
        FENativeInterface::SetString(m_hud, kCountdownText, "GO!");
    } else {
        FENativeInterface::SetVisible(m_hud, kCountdownText, false);
    }

    // Speedometer (km/h, the PAL default) and the tachometer. A drag race shows its manual gearbox
    // (m_gear / m_rpm); otherwise the car's own automatic, or a stand-in without a database record.
    const float speed = std::fabs(car.Speed());
    snprintf(text, sizeof(text), "%d", int(speed * (m_mph ? 2.236936f : 3.6f) + 0.5f));
    FENativeInterface::SetString(m_hud, kSpeed, text);
    FENativeInterface::SetString(m_hud, 0xc3383b63, m_mph ? "MPH" : "KM/H"); // the units label (Options > Speedometer Units)
    float rpm;
    if (IsDrag()) {
        rpm = m_rpm;
        snprintf(text, sizeof(text), "%d", std::clamp(m_gear, 1, kGears));
        FENativeInterface::SetString(m_hud, kGear, text);
    } else if (car.GetPerformance().Valid()) {
        // The car's drivetrain from its database record (PlayerCar::UpdateEngine, ISSUES.md #120).
        rpm = car.Rpm() * 8000.0f / car.GetPerformance().maxRpm; // needle scale: 8000 = full sweep
        if (car.Gear() < 0)
            FENativeInterface::SetString(m_hud, kGear, "R");
        else if (speed < 0.5f && car.Gear() == 1)
            FENativeInterface::SetString(m_hud, kGear, "N");
        else {
            snprintf(text, sizeof(text), "%d", car.Gear());
            FENativeInterface::SetString(m_hud, kGear, text);
        }
    } else {
        constexpr float kGearSpan = 11.0f; // m/s per gear
        int gear = std::min(6, 1 + int(speed / kGearSpan));
        rpm = car.Speed() < -0.5f ? 1000.0f + speed / 12.0f * 5000.0f
                                  : 1000.0f + std::fmod(speed, kGearSpan) / kGearSpan * 6500.0f;
        if (gear == 6 && speed > 6 * kGearSpan)
            rpm = 7500.0f;
        const char *gearText = car.Speed() < -0.5f ? "R" : speed < 0.5f ? "N" : nullptr;
        if (gearText)
            FENativeInterface::SetString(m_hud, kGear, gearText);
        else {
            snprintf(text, sizeof(text), "%d", gear);
            FENativeInterface::SetString(m_hud, kGear, text);
        }
    }
    // The tachometer's textures are per-car "custom" ones in GLOBAL\\HUD_CUSTOMTEXTURES_ALL.BIN: the dial face by redline
    // (3RDPERSON_7500 / 8500 / 10500LINES_CUSTOM_<n>), the needle / dial (3RDPERSON_RPMDIAL_CUSTOM_<n>) and the red fill
    // (TACH_FILL_CUSTOM_<n>); the package's own defaults name textures that are not in any pack (ISSUES #260).
    if (!m_gaugeSkinned) {
        m_gaugeSkinned = true;
        const float maxRpm = car.GetPerformance().Valid() ? car.GetPerformance().maxRpm : 8000.0f;
        const int lines = maxRpm <= 8000.0f ? 7500 : maxRpm <= 9500.0f ? 8500 : 10500;
        m_gaugeLines = lines;
        char name[64];
        auto hashOf = [&](const char *fmt, int a) {
            snprintf(name, sizeof(name), fmt, a);
            return CarMaterial::NameHash(name);
        };
        const int style = getenv("NFSU2_GAUGE_STYLE") ? atoi(getenv("NFSU2_GAUGE_STYLE")) : 0;
        char face[64], fill[64], dial[64];
        snprintf(face, sizeof(face), "3RDPERSON_%dLINES_CUSTOM_%02d", lines, style);
        snprintf(fill, sizeof(fill), "TACH_FILL_CUSTOM_%02d", style);
        snprintf(dial, sizeof(dial), "3RDPERSON_RPMDIAL_CUSTOM_%02d", style);
        (void)hashOf;
        FENativeInterface::SetTextureHash(m_hud, 0x1d9051aa, CarMaterial::NameHash(face));
        FENativeInterface::SetTextureHash(m_hud, 0x05d19f25, CarMaterial::NameHash(fill));
        FENativeInterface::SetTextureHash(m_hud, kNeedle, CarMaterial::NameHash(dial));
        printf("[Hud] gauge %s / %s / %s (redline %.0f)\n", face, fill, dial, maxRpm);
    }
    // Needle sweep as MW's CalcAngleForRPM (66..294 degrees over 0..max rpm).
    if (FEObject *needle = m_hud->FindObjectByHash(kNeedle))
        if (FEObjData *d = needle->GetObjData()) {
            // The needle sprite points straight down at rotation 0 (the dial's zero) and a rotation of theta turns it
            // theta degrees clockwise on screen... measured: theta points it at (180 + theta) degrees clockwise from the top.
            // The face spans ~250 degrees from 0 to its full scale (7500 / 8500 / 10500).
            const float actual = (car.GetPerformance().Valid() && !IsDrag()) ? rpm * car.GetPerformance().maxRpm / 8000.0f : rpm;
            const float factor = std::clamp(actual / float(m_gaugeLines), 0.0f, 1.03f);
            float angle = factor * 250.0f * 3.14159265f / 180.0f;
            if (const char *nt = getenv("NFSU2_NEEDLE_TEST"))
                angle = float(atof(nt)) * 3.14159265f / 180.0f;
            d->Rot.x = d->Rot.y = 0.0f;
            d->Rot.z = std::sin(angle * 0.5f);
            d->Rot.w = std::cos(angle * 0.5f);
        }
    // Nitrous meter (ISSUES #217): shown when the car carries a tank.
    {
        const bool nos = car.HasNitrous();
        FENativeInterface::SetVisible(m_hud, kNosLine, nos);
        FENativeInterface::SetVisible(m_hud, kNosFill, nos);
        // The red arc (script-driven in the original) is cropped from the left to the tank's fill.
        if (nos)
            if (FEObject *fill = m_hud->FindObjectByHash(kNosFill))
                if (FEImage *img = fill->Type == FE_Image ? static_cast<FEImage *>(fill) : nullptr)
                    if (FEImageData *d = img->GetImageData()) {
                        if (m_nosBaseW <= 0.0f) {
                            m_nosBaseW = d->Size.x;
                            m_nosBaseX = d->Pos.x;
                            m_nosBaseU = d->UpperLeft.x;
                            m_nosBaseU1 = d->LowerRight.x;
                        }
                        const char *force = getenv("NFSU2_NOS_FORCE"); // dev aid: pin the displayed fraction
                        const float f = std::clamp(force ? float(atof(force)) : car.NitrousFraction(), 0.0f, 1.0f);
                        d->Size.x = m_nosBaseW * std::max(f, 0.001f);
                        d->Pos.x = m_nosBaseX - m_nosBaseW * 0.5f + d->Size.x * 0.5f;
                        d->UpperLeft.x = m_nosBaseU;
                        d->LowerRight.x = m_nosBaseU + (m_nosBaseU1 - m_nosBaseU) * f;
                        static int n = 0;
                        if (getenv("NFSU2_NOS_DEBUG") && ++n % 60 == 0)
                            printf("[Nos] fraction %.2f active %d level %d size %.1f uv %.2f..%.2f\n", car.NitrousFraction(), car.NitrousActive(), car.NitrousLevel(), d->Size.x, d->UpperLeft.x, d->LowerRight.x);
                    }
    }
    // Drag shift light (part of the HUD package, ISSUES #106): on near the limiter.
    FENativeInterface::SetVisible(m_hud, kShiftLight, IsDrag() && m_phase == kRacing && ShiftLight());

    // Drift score (ISSUES #116): the current chain, its multiplier, the banked total and the slip
    // angle. Comment_DATA carries the TooSlowToGain / LosingMult feedback text.
    if (IsDrift()) {
        snprintf(text, sizeof(text), "%d", int(DriftPoints()));
        FENativeInterface::SetString(m_hud, kDriftPoint, text);
        FENativeInterface::SetString(m_hud, kDriftSpeedData, text);
        snprintf(text, sizeof(text), "X%d", m_driftMult);
        FENativeInterface::SetString(m_hud, kDriftMult, text);
        snprintf(text, sizeof(text), "%d", int(m_driftTotal));
        FENativeInterface::SetString(m_hud, kDriftTotal, text);
        FENativeInterface::SetString(m_hud, kDriftBestTime,
                                     m_bestLap > 0 ? FormatTime(m_bestLap).c_str() : "--:--.--");
        snprintf(text, sizeof(text), "%d", int(std::fabs(m_driftAngle)));
        FENativeInterface::SetString(m_hud, kDriftAngleDeg, text);
        FENativeInterface::SetString(m_hud, kDriftAngleData, text);
        const char *comment = m_phase == kFinished ? "" : m_driftChain > 0 ? "LosingMult"
                              : m_driftAngle > kDriftMinAngle ? "TooSlowToGain" : "";
        FENativeInterface::SetString(m_hud, kCommentData, comment);
    }

    // Out-run get-away meter (ISSUES #119): the banner, the Ahead/Behind label, the gap in metres and
    // the bar filled to the fraction of the escape distance reached.
    if (IsOutrun()) {
        FENativeInterface::SetString(m_hud, kOutRunMessage, "OUTRUN");
        FENativeInterface::SetString(m_hud, kDistanceText, m_outrunLead >= 0 ? "Ahead" : "Behind");
        snprintf(text, sizeof(text), "%.0f", std::fabs(m_outrunLead));
        FENativeInterface::SetString(m_hud, kDistanceNumber, text);
        if (FEObject *bar = m_hud->FindObjectByHash(kGetawayBar))
            if (FEObjData *d = bar->GetObjData()) {
                float frac = std::clamp(std::fabs(m_outrunLead) / kOutrunEscape, 0.0f, 1.0f);
                d->Size.x = kGetawayBarWidth * frac;
            }
    }

    // Lap counter and lap time (free run has neither, ISSUES #118).
    if (IsFreeRun())
        return;
    snprintf(text, sizeof(text), "%d", std::min(m_lap, m_config.laps));
    FENativeInterface::SetString(m_hud, kLapNumber, text);
    FENativeInterface::SetString(m_hud, kCurrentLapTime, FormatTime(m_lapTime).c_str());

    snprintf(text, sizeof(text), "%d", m_position);
    FENativeInterface::SetString(m_hud, kPositionNumber, text);
    static const char *const kNth[] = {"th", "st", "nd", "rd"};
    FENativeInterface::SetString(m_hud, kPositionNth, kNth[m_position >= 1 && m_position <= 3 ? m_position : 0]);
    FENativeInterface::SetVisible(m_hud, kWrongWay, m_wrongWay > 1.0f && m_phase == kRacing);
    // URL: the racer count on the position display drops as cars are knocked out.
    if (IsKnockout()) {
        snprintf(text, sizeof(text), "/%d", ActiveRacers());
        FENativeInterface::SetString(m_hud, kNumberRacers, text);
    }

    // URL standings board: the running order, one row per remaining racer (knocked-out racers drop
    // off). The row children are anonymous, so they are addressed by their ordinal hash.
    if (IsKnockout()) {
        for (int r = 0; r < 4; ++r) {
            const bool have = r < int(m_leaderboard.size());
            FENativeInterface::SetVisible(m_hud, kBoardPos[r], have);
            FENativeInterface::SetVisible(m_hud, kBoardName[r], have);
            FENativeInterface::SetVisible(m_hud, kBoardPlate[r], have);
            FENativeInterface::SetVisible(m_hud, kBoardIcon[r], have);
            if (!have)
                continue;
            int id = m_leaderboard[r];
            snprintf(text, sizeof(text), "%d", r + 1);
            FENativeInterface::SetString(m_hud, kBoardPos[r], text);
            if (id == 0)
                FENativeInterface::SetString(m_hud, kBoardName[r], "YOU");
            else {
                snprintf(text, sizeof(text), "CPU %d", id);
                FENativeInterface::SetString(m_hud, kBoardName[r], text);
            }
        }
    }

    const bool over = m_phase == kFinished;
    FENativeInterface::SetVisible(m_hud, kRaceOverMessage, over);
    FENativeInterface::SetVisible(m_hud, kRaceOverBacking, over);
    if (over) {
        static const char *const kNth[] = {"th", "st", "nd", "rd"};
        if (IsDrift())
            snprintf(text, sizeof(text), "DRIFT %d PTS", int(m_driftTotal));
        else if (IsOutrun())
            snprintf(text, sizeof(text), m_outrunCaught ? "CAUGHT" : "ESCAPED");
        else if (IsKnockout() && m_playerOut)
            snprintf(text, sizeof(text), "KNOCKED OUT");
        else
            snprintf(text, sizeof(text), "%d%s PLACE  %s", m_position,
                     kNth[m_position >= 1 && m_position <= 3 ? m_position : 0], FormatTime(m_totalTime).c_str());
        FENativeInterface::SetString(m_hud, kRaceOverMessage, text);
    }
}
