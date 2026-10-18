#include "game/indep/World/AiDriver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/RaceRoute.h"

void AiDriver::Init(PlayerCar *car, const RaceRoute *route, float lane, float skill)
{
    m_car = car;
    m_route = route;
    m_lane = lane;
    m_skill = skill;
    m_s = route->Project(car->X(), car->Y());
    m_progress = 0;
    m_steer = 0;
    m_stuck = 0;
}

static float kUg2Max = getenv("NFSU2_AI_MAX") ? atof(getenv("NFSU2_AI_MAX")) : 40.0f;
static float kUg2Drop = getenv("NFSU2_AI_DROP") ? atof(getenv("NFSU2_AI_DROP")) : 32.0f;

static float kUg2Look = getenv("NFSU2_AI_LOOK") ? atof(getenv("NFSU2_AI_LOOK")) : 1.2f;

float AiDriver::Update(const TrackWorld &world, float dt, bool go)
{
    PlayerCar &car = *m_car;
    float s = m_route->Project(car.X(), car.Y(), m_s);
    float advanced = m_route->Delta(m_s, s);
    m_progress += advanced;
    m_s = s;

    float throttle = 0, brake = 0, steer = 0; // waiting: no input (braking at rest reverses)
    if (go) {
        const float speed = std::max(0.0f, car.Speed());
        // Target: a point ahead on the route, shifted into this driver's lane.
        float x, y, dx, dy;
        m_route->Sample(s + 12.0f + speed * 0.55f, x, y, dx, dy);
        x += -dy * m_lane;
        y += dx * m_lane;
        float want = std::atan2(y - car.Y(), x - car.X());
        float err = std::remainder(want - car.Yaw(), 6.2831853f);
        steer = std::clamp(err * 2.2f, -1.0f, 1.0f);
        // Speed from the bend ahead: angle between the route chords 10->35 m and 35->60 m (chords
        // smooth out the 25-30 m node spacing).
        float p0x, p0y, p1x, p1y, p2x, p2y, tx, ty;
        // Real brakes need room: look further ahead the faster the car goes.
        const float la = car.UsingUG2Physics() ? speed * kUg2Look : 0.0f;
        m_route->Sample(s + 10.0f + la, p0x, p0y, tx, ty);
        m_route->Sample(s + 35.0f + la, p1x, p1y, tx, ty);
        m_route->Sample(s + 60.0f + la, p2x, p2y, tx, ty);
        float c0x = p1x - p0x, c0y = p1y - p0y, c1x = p2x - p1x, c1y = p2y - p1y;
        float norm = std::max(1e-3f, std::hypot(c0x, c0y) * std::hypot(c1x, c1y));
        float bend = std::acos(std::clamp((c0x * c1x + c0y * c1y) / norm, -1.0f, 1.0f));
        // The real tyre model (UG2 physics) is grip-limited, so the bend speeds are lower than the
        // arcade car's.
        const float vmax = car.UsingUG2Physics() ? kUg2Max : 52.0f, vdrop = car.UsingUG2Physics() ? kUg2Drop : 44.0f;
        float target = m_skill * (vmax - vdrop * std::min(1.0f, bend / 1.05f));
        if (speed < target)
            throttle = 1.0f;
        else if (speed > target + 3.0f)
            brake = std::min(1.0f, (speed - target) / 10.0f);
        // Unstick: if blocked, reverse briefly with opposite lock.
        m_stuck = advanced < 0.05f * dt * 10.0f && speed < 1.0f ? m_stuck + dt : 0.0f;
        if (m_stuck > 1.5f) {
            throttle = 0;
            brake = 1.0f;
            steer = -steer;
            if (m_stuck > 3.0f)
                m_stuck = 0;
        }
    }
    m_steer += std::clamp(steer - m_steer, -4.0f * dt, 4.0f * dt);
    car.Update(world, throttle, brake, m_steer, dt, /*walls=*/false, m_power);
    return m_progress;
}
