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
} // namespace

void RaceSession::Begin(FEPackage *hud, const Config &config)
{
    m_hud = hud;
    m_config = config;
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
    SetupHud();
    printf("[Race] begin: %d laps, track %.0f m, line %s\n", config.laps, config.trackLength,
           config.hasLine ? "from markers" : "none");
}

void RaceSession::SetupHud()
{
    if (!m_hud)
        return;
    // Show only the single-race elements; the package also holds online, career, drag, drift and
    // world-map widgets that UG2's HUD class hides per mode.
    static const uint32_t kShown[] = {kGaugeCluster, kGaugeBacking, kMinimapMask, kMinimapNorth, kTrackMap,
                                      kPosition, kPositionBacking, kLapGroup, kLapTimeGroup, kCountdownText};
    for (FEObject *o = m_hud->GetFirstObject(); o; o = o->GetNext())
        FENativeInterface::SetObjectVisible(o, std::find(std::begin(kShown), std::end(kShown), o->NameHash) !=
                                                   std::end(kShown));
    for (uint32_t h : {kTurboGroup, kNosLine, kNosFill, kNosGameBreaker, kShiftLight})
        FENativeInterface::SetVisible(m_hud, h, false);
    char text[16];
    snprintf(text, sizeof(text), "/%d", m_config.laps);
    FENativeInterface::SetString(m_hud, kNumberLaps, text);
    snprintf(text, sizeof(text), "/%d", m_config.racers);
    FENativeInterface::SetString(m_hud, kNumberRacers, text);
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
    if (m_phase == kRacing && dist >= m_lap * route.Length())
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

    // Speedometer (km/h, the PAL default) and a stand-in automatic gearbox for gear and needle.
    const float speed = std::fabs(car.Speed());
    snprintf(text, sizeof(text), "%d", int(speed * 3.6f + 0.5f));
    FENativeInterface::SetString(m_hud, kSpeed, text);
    constexpr float kGearSpan = 11.0f; // m/s per gear
    int gear = std::min(6, 1 + int(speed / kGearSpan));
    float rpm = car.Speed() < -0.5f ? 1000.0f + speed / 12.0f * 5000.0f
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
    // Needle sweep as MW's CalcAngleForRPM (66..294 degrees over 0..max rpm).
    if (FEObject *needle = m_hud->FindObjectByHash(kNeedle))
        if (FEObjData *d = needle->GetObjData()) {
            float factor = std::clamp(rpm / 8000.0f, 0.0f, 1.0f);
            float angle = (66.0f + factor * (294.0f - 66.0f)) * 3.14159265f / 180.0f;
            d->Rot.x = d->Rot.y = 0.0f;
            d->Rot.z = std::sin(angle * 0.5f);
            d->Rot.w = std::cos(angle * 0.5f);
        }

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
