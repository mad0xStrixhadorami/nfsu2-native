#include "game/indep/World/RaceSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Speed/Indep/Src/FEng/FEGroup.h"
#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "game/indep/Frontend/FENative.h"
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
} // namespace

void RaceSession::Begin(FEPackage *hud, const Config &config)
{
    m_hud = hud;
    m_config = config;
    if (m_config.route && m_config.route->Valid() && !m_config.route->Closed())
        m_config.laps = 1; // sprint
    m_phase = kCountdown;
    m_countdown = 3.0f;
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
    const bool sprint = m_config.route && m_config.route->Valid() && !m_config.route->Closed();
    for (FEObject *o = m_hud->GetFirstObject(); o; o = o->GetNext())
        FENativeInterface::SetObjectVisible(o, std::find(std::begin(kShown), std::end(kShown), o->NameHash) !=
                                                   std::end(kShown) && !(sprint && o->NameHash == kLapGroup));
    for (uint32_t h : {kTurboGroup, kNosLine, kNosFill, kNosGameBreaker, kShiftLight})
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

void RaceSession::CompleteLap()
{
    printf("[Race] lap %d: %s\n", m_lap, FormatTime(m_lapTime).c_str());
    m_bestLap = m_bestLap > 0 ? std::min(m_bestLap, m_lapTime) : m_lapTime;
    m_lapDistance = 0;
    m_lapTime = 0;
    if (++m_lap > m_config.laps) {
        m_phase = kFinished;
        printf("[Race] finished %d of %d in %s (best lap %s)\n", m_position, m_config.racers,
               FormatTime(m_totalTime).c_str(), FormatTime(m_bestLap).c_str());
    }
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

// Progress along the decoded route: laps, finish, position and wrong way.
void RaceSession::UpdateRoute(const PlayerCar &car, const std::vector<float> &opponents, float dt)
{
    const RaceRoute &route = *m_config.route;
    if (m_routeS < 0) {
        m_routeS = m_startS = route.Project(car.X(), car.Y());
        m_progress = 0;
    }
    float s = route.Project(car.X(), car.Y(), m_routeS);
    m_progress += route.Delta(m_routeS, s);
    m_routeS = s;
    const float dist = RaceDistance(m_startS, m_progress);
    if (m_phase == kRacing && dist >= m_lap * LapLength())
        CompleteLap();
    // Position: opponents further along the route are ahead.
    int ahead = 0;
    for (float o : opponents)
        ahead += o > dist;
    if (m_phase == kRacing)
        m_position = 1 + ahead;
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

void RaceSession::UpdateHud(const PlayerCar &car)
{
    if (!m_hud)
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

    // Speedometer (km/h, the PAL default) and the tachometer. A drag race shows the real manual
    // gearbox (m_gear / m_rpm); otherwise a stand-in automatic for gear and needle.
    const float speed = std::fabs(car.Speed());
    snprintf(text, sizeof(text), "%d", int(speed * 3.6f + 0.5f));
    FENativeInterface::SetString(m_hud, kSpeed, text);
    float rpm;
    if (IsDrag()) {
        rpm = m_rpm;
        snprintf(text, sizeof(text), "%d", std::clamp(m_gear, 1, kGears));
        FENativeInterface::SetString(m_hud, kGear, text);
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
    // Needle sweep as MW's CalcAngleForRPM (66..294 degrees over 0..max rpm).
    if (FEObject *needle = m_hud->FindObjectByHash(kNeedle))
        if (FEObjData *d = needle->GetObjData()) {
            float factor = std::clamp(rpm / 8000.0f, 0.0f, 1.0f);
            float angle = (66.0f + factor * (294.0f - 66.0f)) * 3.14159265f / 180.0f;
            d->Rot.x = d->Rot.y = 0.0f;
            d->Rot.z = std::sin(angle * 0.5f);
            d->Rot.w = std::cos(angle * 0.5f);
        }
    // Drag shift light (part of the HUD package, ISSUES #106): on near the limiter.
    FENativeInterface::SetVisible(m_hud, kShiftLight, IsDrag() && m_phase == kRacing && ShiftLight());

    // Lap counter and lap time.
    snprintf(text, sizeof(text), "%d", std::min(m_lap, m_config.laps));
    FENativeInterface::SetString(m_hud, kLapNumber, text);
    FENativeInterface::SetString(m_hud, kCurrentLapTime, FormatTime(m_lapTime).c_str());

    snprintf(text, sizeof(text), "%d", m_position);
    FENativeInterface::SetString(m_hud, kPositionNumber, text);
    static const char *const kNth[] = {"th", "st", "nd", "rd"};
    FENativeInterface::SetString(m_hud, kPositionNth, kNth[m_position >= 1 && m_position <= 3 ? m_position : 0]);
    FENativeInterface::SetVisible(m_hud, kWrongWay, m_wrongWay > 1.0f && m_phase == kRacing);

    const bool over = m_phase == kFinished;
    FENativeInterface::SetVisible(m_hud, kRaceOverMessage, over);
    FENativeInterface::SetVisible(m_hud, kRaceOverBacking, over);
    if (over) {
        static const char *const kNth[] = {"th", "st", "nd", "rd"};
        snprintf(text, sizeof(text), "%d%s PLACE  %s", m_position,
                 kNth[m_position >= 1 && m_position <= 3 ? m_position : 0], FormatTime(m_totalTime).c_str());
        FENativeInterface::SetString(m_hud, kRaceOverMessage, text);
    }
}
