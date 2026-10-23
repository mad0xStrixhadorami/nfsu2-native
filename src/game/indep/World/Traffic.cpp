#include "game/indep/World/Traffic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/indep/World/TrackWorld.h"

unsigned Traffic::Rand()
{
    m_seed = m_seed * 1664525u + 1013904223u;
    return m_seed >> 8;
}

void Traffic::Init(const TrafficNet *net, std::vector<PlayerCar> *cars, unsigned seed)
{
    m_net = net;
    m_seed = seed ? seed : 1;
    m_drivers.assign(cars->size(), Driver());
    for (size_t i = 0; i < cars->size(); ++i) {
        m_drivers[i].car = &(*cars)[i];
        m_drivers[i].cruise = 11.0f + float(Rand() % 800) / 100.0f; // 11..19 m/s
        m_drivers[i].link = -1;
    }
}

void Traffic::Respawn(Driver &d, const TrackWorld &world, float px, float py, bool initial)
{
    const auto &links = m_net->Links();
    const float minD = initial ? 70.0f : 140.0f, maxD = initial ? 320.0f : 300.0f;
    for (int attempt = 0; attempt < 64; ++attempt) {
        int i = int(Rand() % links.size());
        const TrafficNet::Link &l = links[size_t(i)];
        if (l.next < 0 || l.length < 8.0f)
            continue;
        float t = float(Rand() % 1000) / 1000.0f * l.length;
        float x, y, dx, dy;
        m_net->Sample(i, t, x, y, dx, dy);
        float dist = std::hypot(x - px, y - py);
        if (dist < minD || dist > maxD)
            continue;
        bool clear = true;
        for (const Driver &o : m_drivers)
            if (&o != &d && o.link >= 0 && std::hypot(o.car->X() - x, o.car->Y() - y) < 25.0f)
                clear = false;
        if (!clear)
            continue;
        float z = 0;
        if (!world.GroundHeight(x, y, m_pz + 25.0f, z))
            continue;
        d.link = i;
        d.t = t;
        d.steer = 0;
        d.car->Place(x, y, z, std::atan2(dy, dx));
        return;
    }
    d.link = -1; // no room this frame; try again next
}

void Traffic::Update(const TrackWorld &world, float dt, const std::vector<const PlayerCar *> &others)
{
    if (!m_net || !m_net->Valid() || others.empty())
        return;
    m_clock += dt;
    m_px = others[0]->X();
    m_py = others[0]->Y();
    m_pz = others[0]->Z();
    const auto &links = m_net->Links();
    if (getenv("NFSU2_TRAFFIC_DEBUG") && int(m_clock) != m_lastDebug && int(m_clock) % 5 == 0) {
        m_lastDebug = int(m_clock);
        int stopped = 0;
        for (const Driver &d : m_drivers)
            stopped += d.link >= 0 && d.car->Speed() < 0.8f;
        printf("[Traffic] t=%d: %d of %zu stopped\n", m_lastDebug, stopped, m_drivers.size());
    }
    for (Driver &d : m_drivers) {
        PlayerCar &car = *d.car;
        bool initial = d.link < 0 && car.X() == 0.0f && car.Y() == 0.0f;
        if (d.link < 0 || std::hypot(car.X() - m_px, car.Y() - m_py) > 360.0f) {
            Respawn(d, world, m_px, m_py, initial);
            if (d.link < 0)
                continue;
        }
        // Progress along the lane: project onto the current link, hop forward past its end.
        for (int guard = 0; guard < 4; ++guard) {
            const TrafficNet::Link &l = links[size_t(d.link)];
            float dxl = (l.x1 - l.x0) / std::max(l.length, 1e-3f), dyl = (l.y1 - l.y0) / std::max(l.length, 1e-3f);
            d.t = (car.X() - l.x0) * dxl + (car.Y() - l.y0) * dyl;
            if (d.t < l.length - 1.0f)
                break;
            if (l.next < 0) {
                d.link = -1;
                break;
            }
            d.link = l.next;
        }
        if (d.link < 0)
            continue;
        // Aim a little ahead along the chain.
        float ahead = 6.0f + car.Speed() * 0.6f, t = d.t + ahead;
        int li = d.link;
        while (t > links[size_t(li)].length && links[size_t(li)].next >= 0) {
            t -= links[size_t(li)].length;
            li = links[size_t(li)].next;
        }
        float tx, ty, dx, dy;
        m_net->Sample(li, std::min(t, links[size_t(li)].length), tx, ty, dx, dy);
        float err = std::remainder(std::atan2(ty - car.Y(), tx - car.X()) - car.Yaw(), 6.2831853f);
        float steerWant = std::clamp(err * 2.5f, -1.0f, 1.0f);
        d.steer += std::clamp(steerWant - d.steer, -4.0f * dt, 4.0f * dt);
        // Slow for tight bends and for any car close in front.
        float target = d.cruise * (1.0f - 0.65f * std::min(1.0f, std::fabs(err) / 0.7f));
        for (const PlayerCar *o : others) {
            float ox = o->X() - car.X(), oy = o->Y() - car.Y();
            float fwd = ox * std::cos(car.Yaw()) + oy * std::sin(car.Yaw());
            float side = -ox * std::sin(car.Yaw()) + oy * std::cos(car.Yaw());
            if (fwd > 0 && fwd < 22.0f && std::fabs(side) < 3.0f)
                target = std::min(target, std::max(0.0f, (fwd - 7.0f) * 0.6f));
        }
        if (m_net->NearJunction(car.X(), car.Y(), 22.0f))
            target = std::min(target, 7.0f); // ease through junctions
        // Keep a gap to other traffic ahead in the same lane (the loop above covers only the player and rivals).
        for (const Driver &o : m_drivers) {
            if (&o == &d || o.link < 0)
                continue;
            const float ox = o.car->X() - car.X(), oy = o.car->Y() - car.Y();
            const float fwd = ox * std::cos(car.Yaw()) + oy * std::sin(car.Yaw());
            const float side = -ox * std::sin(car.Yaw()) + oy * std::cos(car.Yaw());
            if (fwd > 0 && fwd < 22.0f && std::fabs(side) < 3.0f)
                target = std::min(target, std::max(0.0f, (fwd - 7.0f) * 0.6f));
        }
        // Signals (stand-in for TrafficIntersection, #220): each junction gives its direction axis the
        // green for 9 s, then the cross axis; a car approaching on a red axis stops 7 m short.
        const auto &junctions = m_net->Junctions();
        for (size_t ji = 0; ji < junctions.size(); ++ji) {
            const TrafficNet::Junction &j = junctions[ji];
            const float jx = j.x - car.X(), jy = j.y - car.Y();
            const float dist = std::hypot(jx, jy);
            if (dist > 30.0f || dist < 7.0f)
                continue;
            const float hx = std::cos(car.Yaw()), hy = std::sin(car.Yaw());
            if (jx * hx + jy * hy < 0.5f * dist)
                continue; // not heading at it
            const bool alongAxis = std::fabs(hx * j.dx + hy * j.dy) > 0.7f;
            const bool green = ((int(m_clock / 9.0f) + int(ji)) % 2 == 0) == alongAxis;
            if (!green)
                target = std::min(target, std::max(0.0f, (dist - 8.0f) * 0.6f));
        }
        float speed = std::max(0.0f, car.Speed());
        float throttle = speed < target ? 0.6f : 0.0f;
        float brake = speed > target + 1.0f ? std::min(1.0f, (speed - target) / 6.0f) : 0.0f;
        car.Update(world, throttle, brake, d.steer, dt, /*walls=*/false);
    }
}
