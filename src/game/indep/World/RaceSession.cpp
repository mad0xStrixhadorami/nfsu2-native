#include "game/indep/World/RaceSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Speed/Indep/Src/FEng/FEGroup.h"
#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "game/indep/Frontend/FENative.h"
#include "game/indep/World/PlayerCar.h"

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
    FENativeInterface::SetString(m_hud, kPositionNumber, "1");
    FENativeInterface::SetString(m_hud, kPositionNth, "st");
    FENativeInterface::SetString(m_hud, kNumberRacers, "/1"); // no opponents yet
}

bool RaceSession::Update(const PlayerCar &car, float dt)
{
    switch (m_phase) {
    case kCountdown:
        m_countdown -= dt;
        if (m_countdown <= 0) {
            m_phase = kRacing;
            m_goShown = 1.0f;
        }
        break;
    case kRacing: {
        m_lapTime += dt;
        m_totalTime += dt;
        m_goShown = std::max(0.0f, m_goShown - dt);
        if (m_havePrev)
            m_lapDistance += std::hypot(car.X() - m_prevX, car.Y() - m_prevY);
        // Lap: crossing the start/finish line in the direction of travel, near the line, after
        // covering most of a lap (the route itself is not decoded yet, see ISSUES.md #106).
        if (m_config.hasLine) {
            float rx = car.X() - m_config.lineX, ry = car.Y() - m_config.lineY;
            float side = rx * m_config.dirX + ry * m_config.dirY;
            float across = std::fabs(-rx * m_config.dirY + ry * m_config.dirX);
            if (m_havePrev && m_lastSide < 0 && side >= 0 && across < 25.0f &&
                m_lapDistance > 0.6f * m_config.trackLength) {
                printf("[Race] lap %d: %s\n", m_lap, FormatTime(m_lapTime).c_str());
                m_bestLap = m_bestLap > 0 ? std::min(m_bestLap, m_lapTime) : m_lapTime;
                m_lapDistance = 0;
                m_lapTime = 0;
                if (++m_lap > m_config.laps) {
                    m_phase = kFinished;
                    printf("[Race] finished in %s (best lap %s)\n", FormatTime(m_totalTime).c_str(),
                           FormatTime(m_bestLap).c_str());
                }
            }
            m_lastSide = side;
        }
        break;
    }
    case kFinished:
        break;
    }
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

    const bool over = m_phase == kFinished;
    FENativeInterface::SetVisible(m_hud, kRaceOverMessage, over);
    FENativeInterface::SetVisible(m_hud, kRaceOverBacking, over);
    if (over) {
        snprintf(text, sizeof(text), "RACE OVER  %s", FormatTime(m_totalTime).c_str());
        FENativeInterface::SetString(m_hud, kRaceOverMessage, text);
    }
}
