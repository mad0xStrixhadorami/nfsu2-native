#include "game/indep/World/Pursuit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <unordered_map>

#include "game/indep/World/TrackWorld.h"

namespace {
constexpr float kPi = 3.14159265f;

float Angle(float a) { return std::remainder(a, 2.0f * kPi); }

float SegDist(float px, float py, float x0, float y0, float x1, float y1, float &t)
{
    const float dx = x1 - x0, dy = y1 - y0, l2 = std::max(dx * dx + dy * dy, 1e-6f);
    t = std::clamp(((px - x0) * dx + (py - y0) * dy) / l2, 0.0f, 1.0f);
    return std::hypot(px - (x0 + dx * t), py - (y0 + dy * t));
}
} // namespace

unsigned Pursuit::Rand()
{
    m_seed = m_seed * 1664525u + 1013904223u;
    return m_seed >> 8;
}

void Pursuit::Banner(const std::string &text, const std::string &sub)
{
    m_hud.bannerText = text, m_hud.bannerSub = sub, m_hud.banner = 0.001f;
}

void Pursuit::BuildGraph()
{
    // 2N lane segments: the lanes as authored (0..N-1) and every lane driven the other way (N..2N-1): the cops do not keep to the traffic's side.
    const auto &links = m_net->Links();
    const size_t n = links.size();
    m_seg.resize(2 * n);
    for (size_t i = 0; i < n; ++i) {
        const TrafficNet::Link &l = links[i];
        m_seg[i] = {l.x0, l.y0, l.x1, l.y1, l.length};
        m_seg[n + i] = {l.x1, l.y1, l.x0, l.y0, l.length};
    }
    m_graph.assign(2 * n, Node());
    std::unordered_map<int64_t, std::vector<int>> grid;
    auto key = [](float x, float y) { return int64_t(std::floor(x / 8.0f)) * 100003 + int64_t(std::floor(y / 8.0f)); };
    for (size_t i = 0; i < 2 * n; ++i)
        grid[key(m_seg[i].x0, m_seg[i].y0)].push_back(int(i));
    for (size_t i = 0; i < 2 * n; ++i) {
        const Seg &a = m_seg[i];
        const size_t twin = i < n ? i + n : i - n;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                auto it = grid.find(key(a.x1 + float(dx) * 8.0f, a.y1 + float(dy) * 8.0f));
                if (it == grid.end())
                    continue;
                for (int j : it->second) {
                    const Seg &b = m_seg[size_t(j)];
                    if (size_t(j) == i || size_t(j) == twin || std::hypot(b.x0 - a.x1, b.y0 - a.y1) > 3.5f)
                        continue;
                    if (std::find(m_graph[i].succ.begin(), m_graph[i].succ.end(), j) == m_graph[i].succ.end())
                        m_graph[i].succ.push_back(j);
                }
            }
    }
}

int Pursuit::NextOf(int link) const
{
    const TrafficNet::Link &l = m_net->Links()[size_t(link)];
    if (l.next >= 0)
        return l.next;
    std::vector<int> fwd;
    for (int s : m_graph[size_t(link)].succ)
        if (size_t(s) < m_net->Links().size())
            fwd.push_back(s);
    return fwd.empty() ? -1 : fwd[size_t(link) % fwd.size()];
}

int Pursuit::NearestLink(float x, float y, float maxDist, float yaw, bool useYaw) const
{
    int best = -1;
    float bd = maxDist;
    for (size_t i = 0; i < m_seg.size(); ++i) {
        const Seg &l = m_seg[i];
        if (l.length < 1.0f)
            continue;
        float t;
        const float d = SegDist(x, y, l.x0, l.y0, l.x1, l.y1, t);
        if (d >= bd)
            continue;
        if (useYaw) {
            const float dir = std::atan2(l.y1 - l.y0, l.x1 - l.x0);
            if (std::fabs(Angle(dir - yaw)) > 1.2f)
                continue;
        } else if (i >= m_net->Links().size())
            continue; // a position alone names the lane as authored
        bd = d, best = int(i);
    }
    return best;
}

bool Pursuit::Route(int from, int goal, std::vector<int> &out) const
{
    out.clear();
    if (from < 0 || goal < 0)
        return false;
    if (from == goal) {
        out.push_back(from);
        return true;
    }
    const float gx = 0.5f * (m_seg[size_t(goal)].x0 + m_seg[size_t(goal)].x1), gy = 0.5f * (m_seg[size_t(goal)].y0 + m_seg[size_t(goal)].y1);
    const size_t n = m_net->Links().size();
    auto goalNode = [&](int id) { return size_t(id) % n == size_t(goal) % n; }; // either way along the goal lane ends the route
    struct Open {
        float f;
        int id;
        bool operator<(const Open &o) const { return f > o.f; }
    };
    std::priority_queue<Open> open;
    std::unordered_map<int, float> g;
    std::unordered_map<int, int> prev;
    g[from] = 0.0f;
    open.push({std::hypot(m_seg[size_t(from)].x1 - gx, m_seg[size_t(from)].y1 - gy), from});
    int iter = 0;
    while (!open.empty() && iter++ < 9000) {
        const Open cur = open.top();
        open.pop();
        if (goalNode(cur.id)) {
            for (int nd = cur.id; nd != from; nd = prev[nd])
                out.push_back(nd);
            out.push_back(from);
            std::reverse(out.begin(), out.end());
            return true;
        }
        const float gc = g[cur.id];
        for (int nx : m_graph[size_t(cur.id)].succ) {
            const float ng = gc + m_seg[size_t(nx)].length;
            auto it = g.find(nx);
            if (it != g.end() && it->second <= ng)
                continue;
            g[nx] = ng, prev[nx] = cur.id;
            open.push({ng + std::hypot(m_seg[size_t(nx)].x1 - gx, m_seg[size_t(nx)].y1 - gy), nx});
        }
    }
    return false;
}

bool Pursuit::Init(TrackWorld &world, const PoliceMesh &mesh, const PoliceMesh *lodMesh, const uint8_t *record, const TrafficNet *net, int maxUnits)
{
    if (!record || !net || !net->Valid() || mesh.model.empty() || maxUnits <= 0)
        return false;
    m_mesh = &mesh;
    m_net = net;
    BuildGraph();
    for (const PoliceMesh::Tex &t : mesh.textures) {
        std::vector<uint32_t> px = t.rgba;
        for (uint32_t &p : px)
            p |= 0xff000000u;
        world.AddCustomTexture(t.hash, t.w, t.h, std::move(px), t.gloss);
    }
    // A stock car's record with the Skoda's wheels: the physics must see the wheelbase, track and radius of the body it drives.
    std::vector<uint8_t> rec(record, record + 0x600);
    auto put = [&](size_t off, float v) { memcpy(rec.data() + off, &v, 4); };
    for (int i = 0; i < 4; ++i) {
        const size_t wo = 0x128 + 0x30 * size_t(i);
        put(wo, mesh.hubs[i * 3]), put(wo + 4, mesh.hubs[i * 3 + 1]), put(wo + 8, mesh.hubs[i * 3 + 2]);
        put(wo + 0x10, mesh.radius), put(wo + 0x14, mesh.width);
    }
    put(0x118, 0.18f), put(0x11c, 0.0f), put(0x120, mesh.hubs[2] + 0.2f);
    m_units.reserve(size_t(maxUnits));
    for (int i = 0; i < maxUnits; ++i) {
        m_units.emplace_back();
        Unit &u = m_units.back();
        if (i == 0)
            u.car.SetCustomModel(mesh.model, mesh.triTex, mesh.wheelOf, mesh.rims, mesh.hubs, mesh.radius);
        else
            u.car.SetModel(m_units[0].car);
        if (!u.car.UseUG2Physics(rec.data())) {
            printf("[Police] the patched record was refused\n");
            m_units.clear();
            return false;
        }
        u.worldIdx = world.AddCar(&u.car.Model(), &u.car.TriangleTextures(), SDL_Color{16, 16, 18, 255}, &u.car.TriangleRims(), SDL_Color{150, 154, 162, 255},
                                  &u.car.TriangleCalipers(), SDL_Color{128, 128, 128, 255});
        world.SetCarNormals(u.worldIdx, mesh.normals);
        world.SetCarHidden(u.worldIdx, true);

        u.car.Place(1.0e5f, 1.0e5f, 0.0f, 0.0f);
    }
    // the far models go after all the full ones, so that the full ones keep the world indices that follow the cars of the main loop
    if (lodMesh && !lodMesh->model.empty())
        for (Unit &u : m_units) {
            u.lod.SetCustomModel(lodMesh->model, lodMesh->triTex, lodMesh->wheelOf, lodMesh->rims, lodMesh->hubs, lodMesh->radius);
            u.worldLod = world.AddCar(&u.lod.Model(), &u.lod.TriangleTextures(), SDL_Color{16, 16, 18, 255}, &u.lod.TriangleRims(), SDL_Color{150, 154, 162, 255},
                                      &u.lod.TriangleCalipers(), SDL_Color{128, 128, 128, 255});
            world.SetCarNormals(u.worldLod, lodMesh->normals);
            world.SetCarHidden(u.worldLod, true);
            m_hasLod = true;
        }
    printf("[Police] %d cars, %zu triangles each, %zu lane links\n", maxUnits, mesh.model.size(), m_net->Links().size());
    if (getenv("NFSU2_PURSUIT_DEBUG")) {
        int shown = 0;
        for (const TrafficNet::Link &l : m_net->Links())
            if (l.length > 50.0f && std::hypot(l.x0 + 300.0f, l.y0 + 420.0f) < 450.0f && shown++ < 14)
                printf("[Pursuit] lane %.0f %.0f -> %.0f %.0f (%.0f m, class %d)\n", l.x0, l.y0, l.x1, l.y1, l.length, l.laneClass);
    }
    return true;
}

std::vector<PlayerCar *> Pursuit::Cars()
{
    std::vector<PlayerCar *> out;
    for (Unit &u : m_units)
        out.push_back(&u.car);
    return out;
}

bool Pursuit::PlaceOnLink(Unit &u, TrackWorld &world, int link, float t, const PlayerCar &player)
{
    float x, y, dx, dy, z = 0;
    m_net->Sample(link, t, x, y, dx, dy);
    if (!world.GroundHeight(x, y, player.Z() + 25.0f, z))
        return false;
    u.car.Place(x, y, z, std::atan2(dy, dx));
    u.link = link, u.t = t;
    u.route.clear();
    u.goalLink = -2, u.repath = 0, u.stuck = 0, u.reverse = 0, u.steer = 0, u.age = 0, u.lost = 0, u.sees = 0, u.ramCool = 0, u.immobile = 0;
    return true;
}

bool Pursuit::SpawnUnit(TrackWorld &world, const PlayerCar &player, Mode mode, float minD, float maxD, bool behind)
{
    Unit *free = nullptr;
    for (Unit &u : m_units)
        if (u.mode == kOff) {
            free = &u;
            break;
        }
    if (!free)
        return false;
    const auto &links = m_net->Links();
    // every lane whose middle lies in the ring around the player (and, for `behind`, not ahead of the car)
    std::vector<int> cand;
    const float fx = std::cos(player.Yaw()), fy = std::sin(player.Yaw());
    for (size_t i = 0; i < links.size(); ++i) {
        const TrafficNet::Link &l = links[i];
        if (l.length < 10.0f || NextOf(int(i)) < 0)
            continue;
        const float mx = 0.5f * (l.x0 + l.x1), my = 0.5f * (l.y0 + l.y1);
        const float d = std::hypot(mx - player.X(), my - player.Y());
        if (d < minD || d > maxD)
            continue;
        if (behind && (mx - player.X()) * fx + (my - player.Y()) * fy > 0.2f * d)
            continue;
        cand.push_back(int(i));
    }
    for (int attempt = 0; attempt < 24 && !cand.empty(); ++attempt) {
        const int i = cand[Rand() % cand.size()];
        const TrafficNet::Link &l = links[size_t(i)];
        const float t = (0.2f + 0.6f * float(Rand() % 1000) / 1000.0f) * l.length;
        float x, y, dx, dy;
        m_net->Sample(i, t, x, y, dx, dy);
        bool clear = true;
        for (const Unit &o : m_units)
            if (&o != free && o.mode != kOff && std::hypot(o.car.X() - x, o.car.Y() - y) < 30.0f)
                clear = false;
        if (!clear || !PlaceOnLink(*free, world, i, t, player))
            continue;
        free->mode = mode;
        free->siren = mode != kPatrol;
        world.SetCarHidden(free->worldIdx, false);
        return true;
    }
    if (getenv("NFSU2_PURSUIT_DEBUG"))
        printf("[Pursuit] no spawn %.0f..%.0f m (mode %d): %zu lanes in the ring\n", minD, maxD, int(mode), cand.size());
    return false;
}

void Pursuit::Recycle(Unit &u)
{
    u.mode = kOff;
    u.siren = false;
    u.car.SetHold(false);
    u.car.Place(1.0e5f, 1.0e5f, 0.0f, 0.0f);
}

void Pursuit::DriveUnit(Unit &u, TrackWorld &world, float dt, PlayerCar &player, float targetSpeed, float aimX, float aimY, float power)
{
    (void)player;
    PlayerCar &car = u.car;
    if (dt <= 0.0f)
        return;
    const float yaw = car.Yaw(), speed = car.Speed();
    const float desired = std::atan2(aimY - car.Y(), aimX - car.X());
    float err = Angle(desired - yaw);
    float steerWant = std::clamp(err * 2.0f, -1.0f, 1.0f);
    // feelers: steer away from walls ahead
    const float reach = 6.0f + std::max(0.0f, speed) * 0.55f;
    const float z = car.Z() + 0.8f;
    const bool hitL = world.HitsWall(car.X(), car.Y(), z, car.X() + std::cos(yaw + 0.42f) * reach, car.Y() + std::sin(yaw + 0.42f) * reach, z);
    const bool hitR = world.HitsWall(car.X(), car.Y(), z, car.X() + std::cos(yaw - 0.42f) * reach, car.Y() + std::sin(yaw - 0.42f) * reach, z);
    const bool hitC = world.HitsWall(car.X(), car.Y(), z, car.X() + std::cos(yaw) * reach, car.Y() + std::sin(yaw) * reach, z);
    if (hitL && !hitR)
        steerWant -= 0.7f;
    else if (hitR && !hitL)
        steerWant += 0.7f;
    else if (hitC || (hitL && hitR))
        steerWant += (err >= 0 ? 0.6f : -0.6f), targetSpeed = std::min(targetSpeed, 6.0f);
    steerWant = std::clamp(steerWant, -1.0f, 1.0f);
    u.steer += std::clamp(steerWant - u.steer, -4.5f * dt, 4.5f * dt);
    // slow for corners
    if (std::fabs(err) > 0.5f)
        targetSpeed *= std::max(0.4f, 1.0f - (std::fabs(err) - 0.5f) * 0.6f);
    float throttle = speed < targetSpeed ? std::clamp((targetSpeed - speed) * 0.45f, 0.25f, 1.0f) : 0.0f;
    float brake = speed > targetSpeed + 1.5f ? std::clamp((speed - targetSpeed) / 9.0f, 0.0f, 1.0f) : 0.0f;
    // stuck: back out
    if (u.reverse > 0.0f) {
        u.reverse -= dt;
        car.Update(world, 0.0f, 0.9f, std::clamp(-err * 1.5f, -1.0f, 1.0f), dt, true, 1.0f);
        return;
    }
    if (throttle > 0.3f && std::fabs(speed) < 1.2f)
        u.stuck += dt;
    else
        u.stuck = std::max(0.0f, u.stuck - dt * 2.0f);
    if (u.stuck > 1.7f) {
        u.stuck = 0.0f;
        u.reverse = 1.4f;
    }
    car.Update(world, throttle, brake, u.steer, dt, true, power);
}

void Pursuit::UpdatePatrol(Unit &u, TrackWorld &world, float dt, PlayerCar &player)
{
    PlayerCar &car = u.car;
    const auto &links = m_net->Links();
    if (u.link < 0 || std::hypot(car.X() - m_px, car.Y() - m_py) > 460.0f) {
        Recycle(u);
        return;
    }
    for (int guard = 0; guard < 4; ++guard) {
        const TrafficNet::Link &l = links[size_t(u.link)];
        const float dxl = (l.x1 - l.x0) / std::max(l.length, 1e-3f), dyl = (l.y1 - l.y0) / std::max(l.length, 1e-3f);
        u.t = (car.X() - l.x0) * dxl + (car.Y() - l.y0) * dyl;
        if (u.t < l.length - 1.0f)
            break;
        if (NextOf(u.link) < 0) {
            Recycle(u);
            return;
        }
        u.link = NextOf(u.link);
    }
    float ahead = 7.0f + car.Speed() * 0.6f, t = u.t + ahead;
    int li = u.link;
    while (t > links[size_t(li)].length && NextOf(li) >= 0) {
        t -= links[size_t(li)].length;
        li = NextOf(li);
    }
    float tx, ty, dx, dy;
    m_net->Sample(li, std::min(t, links[size_t(li)].length), tx, ty, dx, dy);
    float target = 12.5f;
    if (m_net->NearJunction(car.X(), car.Y(), 22.0f))
        target = 7.0f;
    // nothing but the player to avoid
    const float ox = player.X() - car.X(), oy = player.Y() - car.Y();
    const float fwd = ox * std::cos(car.Yaw()) + oy * std::sin(car.Yaw()), side = -ox * std::sin(car.Yaw()) + oy * std::cos(car.Yaw());
    if (fwd > 0 && fwd < 20.0f && std::fabs(side) < 3.0f)
        target = std::min(target, std::max(0.0f, (fwd - 7.0f) * 0.6f));
    DriveUnit(u, world, dt, player, target, tx, ty, 1.0f);
    // the patrol notices speeding in its sight
    const float d = std::hypot(ox, oy);
    if (m_state == kIdle && m_graceTimer <= 0.0f) {
        const bool los = d < 85.0f && !world.HitsWall(car.X(), car.Y(), car.Z() + 1.0f, m_px, m_py, m_pz + 1.0f);
        if (los && m_pspeed > 31.0f)
            u.sees += dt;
        else
            u.sees = std::max(0.0f, u.sees - dt * 0.5f);
        const float rel = std::fabs(car.Speed() - m_pspeed);
        if (u.sees > 1.1f || (d < 3.6f && rel > 3.0f))
            u.ramCool = 1.0f; // flagged: Update starts the chase
    }
}

void Pursuit::UpdatePursuer(Unit &u, TrackWorld &world, float dt, PlayerCar &player)
{
    PlayerCar &car = u.car;
    const auto &links = m_net->Links();
    u.age += dt;
    u.ramCool = std::max(0.0f, u.ramCool - dt);
    const float dx = m_px - car.X(), dy = m_py - car.Y(), d = std::hypot(dx, dy);
    const bool los = d < 150.0f && !world.HitsWall(car.X(), car.Y(), car.Z() + 1.0f, m_px, m_py, m_pz + 1.0f);
    u.sees = los ? 1.0f : 0.0f;
    u.lost = d > 330.0f ? u.lost + dt : 0.0f;
    if (u.lost > 8.0f) { // far behind: come back in range
        Recycle(u);
        return;
    }
    if (u.mode == kPursue && d > 25.0f && std::fabs(car.Speed()) < 1.5f)
        u.immobile += dt;
    else
        u.immobile = 0.0f;
    if (u.immobile > 7.0f) { // wedged somewhere: a fresh car takes its place
        Recycle(u);
        return;
    }
    const float heatK = float(m_hud.heat);
    float target = 21.0f + 3.2f * heatK;
    float power = 1.0f + 0.04f * heatK + 0.45f * std::clamp((d - 45.0f) / 140.0f, 0.0f, 1.0f);
    float ax = m_px, ay = m_py;
    if (d < 38.0f && los) {
        // close: run the car down, aiming where it is going
        ax = m_px + m_pvx * 0.5f, ay = m_py + m_pvy * 0.5f;
        target = std::max(target, m_pspeed * 1.04f + 2.0f);
        if (d < 12.0f) // along its flank: lean onto it
            target = m_pspeed * 1.05f + 3.0f;
    } else {
        u.repath -= dt;
        const int goal = NearestLink(m_px, m_py, 45.0f, 0.0f, false);
        if ((u.repath <= 0.0f || goal != u.goalLink) && goal >= 0) {
            const int from = NearestLink(car.X(), car.Y(), 30.0f, car.Yaw(), true);
            if (from >= 0 && Route(from, goal, u.route))
                u.goalLink = goal;
            else
                u.route.clear();
            u.repath = 1.5f;
        }
        if (!u.route.empty()) {
            // progress: the first route link whose segment the car is on
            size_t best = 0;
            float bd = 1e9f;
            for (size_t i = 0; i < u.route.size() && i < 8; ++i) {
                const Seg &l = m_seg[size_t(u.route[i])];
                float t;
                const float dd = SegDist(car.X(), car.Y(), l.x0, l.y0, l.x1, l.y1, t);
                if (dd < bd)
                    bd = dd, best = i;
            }
            if (best > 0)
                u.route.erase(u.route.begin(), u.route.begin() + long(best));
            const Seg &l0 = m_seg[size_t(u.route[0])];
            float t0;
            SegDist(car.X(), car.Y(), l0.x0, l0.y0, l0.x1, l0.y1, t0);
            float t = t0 * l0.length + 9.0f + std::max(0.0f, car.Speed()) * 0.55f;
            size_t li = 0;
            while (t > m_seg[size_t(u.route[li])].length && li + 1 < u.route.size()) {
                t -= m_seg[size_t(u.route[li])].length;
                ++li;
            }
            const Seg &sl = m_seg[size_t(u.route[li])];
            const float f = std::min(1.0f, t / std::max(sl.length, 1e-3f));
            ax = sl.x0 + (sl.x1 - sl.x0) * f, ay = sl.y0 + (sl.y1 - sl.y0) * f;
            if (li + 1 >= u.route.size() && t >= sl.length - 4.0f)
                ax = m_px, ay = m_py; // the last lane: the rest is straight at the car
        }
        target = std::max(target, m_pspeed + 3.0f + std::max(0.0f, d - 40.0f) * 0.08f);
        if (m_net->NearJunction(car.X(), car.Y(), 14.0f) && d > 60.0f)
            target = std::min(target, 15.0f);
    }
    target = std::min(target, 58.0f);
    DriveUnit(u, world, dt, player, target, ax, ay, power);
}

void Pursuit::UpdateBlock(Unit &u, TrackWorld &world, float dt, PlayerCar &player)
{
    PlayerCar &car = u.car;
    car.SetHold(true);
    car.Update(world, 0.0f, 0.0f, 0.0f, dt, true, 1.0f);
    const float d = std::hypot(car.X() - m_px, car.Y() - m_py);
    const float fx = std::cos(m_pyaw), fy = std::sin(m_pyaw);
    const bool behind = (car.X() - m_px) * fx + (car.Y() - m_py) * fy < -15.0f;
    u.age += dt;
    (void)player;
    if ((behind && u.age > 4.0f) || d > 420.0f || u.age > 90.0f)
        Recycle(u);
}

void Pursuit::MakeRoadblock(TrackWorld &world, const PlayerCar &player)
{
    // walk the player's lane ahead
    const auto &links = m_net->Links();
    int li = NearestLink(player.X(), player.Y(), 14.0f, player.Yaw(), true);
    if (li < 0)
        return;
    float need = 190.0f + 40.0f * float(Rand() % 100) / 100.0f, along = -0.0f;
    float t0 = 0;
    {
        const TrafficNet::Link &l = links[size_t(li)];
        float t;
        SegDist(player.X(), player.Y(), l.x0, l.y0, l.x1, l.y1, t);
        t0 = t * l.length;
    }
    along = -t0;
    while (along + links[size_t(li)].length < need && NextOf(li) >= 0) {
        along += links[size_t(li)].length;
        li = NextOf(li);
    }
    const float t = std::clamp(need - along, 2.0f, links[size_t(li)].length - 2.0f);
    float x, y, dx, dy;
    m_net->Sample(li, t, x, y, dx, dy);
    const float yaw = std::atan2(dy, dx);
    int placed = 0;
    for (int k = 0; k < 2; ++k) {
        Unit *free = nullptr;
        for (Unit &u : m_units)
            if (u.mode == kOff) {
                free = &u;
                break;
            }
        if (!free)
            break;
        float gz = 0;
        const float off = (k == 0 ? -1.0f : 1.0f) * 1.7f;
        const float cx = x - dy * off, cy = y + dx * off;
        if (!world.GroundHeight(cx, cy, player.Z() + 25.0f, gz))
            continue;
        free->car.Place(cx, cy, gz, yaw + kPi * 0.5f + (k == 0 ? 0.12f : -0.12f));
        free->mode = kBlock, free->age = 0, free->siren = true, free->stuck = 0;
        free->car.SetHold(true);
        world.SetCarHidden(free->worldIdx, false);
        ++placed;
    }
    if (placed)
        Banner("ROADBLOCK AHEAD", "They are closing the road");
}

void Pursuit::Start(TrackWorld &world, PlayerCar &player, int heat)
{
    m_state = kChase;
    m_hud = Hud();
    m_hud.state = kChase;
    m_hud.heat = std::clamp(heat, 1, 5);
    m_hadUnits = false, m_contact = false;
    m_heatTimer = 0, m_unseen = 0, m_spawnTimer = 0, m_roadblockTimer = 25.0f, m_graceTimer = 0;
    m_px = player.X(), m_py = player.Y(), m_pz = player.Z();
    for (Unit &u : m_units) {
        if (u.mode == kPatrol && std::hypot(u.car.X() - m_px, u.car.Y() - m_py) < 380.0f) {
            u.mode = kPursue, u.siren = true, u.route.clear(), u.goalLink = -2, u.repath = 0, m_contact = true, m_hadUnits = true;
        } else if (u.mode == kPatrol)
            Recycle(u);
    }
    Banner("PURSUIT", "The police are after you");
    (void)world;
    printf("[Pursuit] started at heat %d\n", m_hud.heat);
}

bool Pursuit::DevPlacePlayer(TrackWorld &world, PlayerCar &player, int nth)
{
    const auto &links = m_net->Links();
    int seen = 0;
    for (size_t i = 0; i < links.size(); ++i) {
        const TrafficNet::Link &l = links[i];
        if (l.length < 70.0f || NextOf(int(i)) < 0 || std::hypot(l.x0 - player.X(), l.y0 - player.Y()) > 900.0f)
            continue;
        float x, y, dx, dy, z = 0;
        m_net->Sample(int(i), l.length * 0.5f, x, y, dx, dy);
        if (!world.GroundHeight(x, y, player.Z() + 25.0f, z) || std::fabs(z - player.Z()) > 12.0f)
            continue;
        if (seen++ < nth)
            continue;
        player.Place(x, y, z, std::atan2(dy, dx));
        printf("[Pursuit] player placed on lane %zu at %.0f %.0f z %.1f\n", i, x, y, z);
        return true;
    }
    return false;
}

void Pursuit::Dismiss()
{
    for (Unit &u : m_units)
        if (u.mode != kOff)
            Recycle(u);
    m_state = kIdle;
    m_hud = Hud();
    m_graceTimer = 30.0f;
}

float Pursuit::NearestSiren() const
{
    float best = -1.0f;
    for (const Unit &u : m_units)
        if (u.mode != kOff && u.siren) {
            const float d = std::hypot(u.car.X() - m_px, u.car.Y() - m_py);
            if (best < 0.0f || d < best)
                best = d;
        }
    return best;
}

bool Pursuit::TakeResult(bool &busted, float &amount)
{
    if (m_resultTaken || !ResultPending())
        return false;
    m_resultTaken = true;
    busted = m_resultBusted, amount = m_resultAmount;
    return true;
}

void Pursuit::Update(TrackWorld &world, float dt, PlayerCar &player, bool racing)
{
    if (m_units.empty())
        return;
    if (!m_enabled || !racing) {
        return;
    }
    m_px = player.X(), m_py = player.Y(), m_pz = player.Z(), m_pyaw = player.Yaw(), m_pspeed = std::fabs(player.Speed());
    m_pvx = std::cos(m_pyaw) * player.Speed(), m_pvy = std::sin(m_pyaw) * player.Speed();
    if (dt <= 0.0f)
        return;
    m_clock += dt;
    // which model each car shows: the full one within 32 m, the rough one up to 190 m, nothing beyond (the transform of a hidden car costs nothing)
    for (Unit &u : m_units) {
        const float d = u.mode == kOff ? 1e9f : std::hypot(u.car.X() - m_px, u.car.Y() - m_py);
        const bool near = d < 32.0f || !m_hasLod, far = !near && d < 190.0f;
        world.SetCarHidden(u.worldIdx, !(u.mode != kOff && near));
        if (m_hasLod)
            world.SetCarHidden(u.worldLod, !(u.mode != kOff && far));
    }
    m_graceTimer = std::max(0.0f, m_graceTimer - dt);
    if (m_hud.banner > 0.0f) {
        m_hud.banner += dt;
        if (m_hud.banner > 3.2f)
            m_hud.banner = 0.0f;
    }

    // dev aid NFSU2_POLICE_SHOW=<ahead>,<left>,<yaw offset>: one parked cop at that place relative to the player, siren on (to look at the car)
    if (const char *show = getenv("NFSU2_POLICE_SHOW")) {
        float ahead = 8, left = 3, yawOff = 3.14159f;
        sscanf(show, "%f,%f,%f", &ahead, &left, &yawOff);
        Unit &u = m_units[0];
        if (u.mode == kOff) {
            const float cy = std::cos(m_pyaw), sy = std::sin(m_pyaw);
            const float x = m_px + cy * ahead - sy * left, y = m_py + sy * ahead + cy * left;
            float z = m_pz;
            world.GroundHeight(x, y, m_pz + 3.0f, z);
            u.car.Place(x, y, z, m_pyaw + yawOff);
            u.mode = kBlock, u.siren = true, u.age = 0;
            world.SetCarHidden(u.worldIdx, false);
            u.car.SetHold(true);
            printf("[Pursuit] show cop at %.1f %.1f z %.1f (player %.1f %.1f z %.1f) yaw %.2f\n", x, y, z, m_px, m_py, m_pz, m_pyaw + yawOff);
        } else {
            u.age = 0;
            u.car.Update(world, 0.0f, 0.0f, 0.0f, dt, true, 1.0f);
        }
        return;
    }
    // ---- idle: patrols cruise the lanes
    if (m_state == kIdle) {
        int patrols = 0;
        for (Unit &u : m_units)
            patrols += u.mode == kPatrol;
        m_spawnPatrol -= dt;
        if (patrols < 2 && m_spawnPatrol <= 0.0f && m_graceTimer <= 0.0f) {
            SpawnUnit(world, player, kPatrol, 160.0f, 380.0f, false);
            m_spawnPatrol = 4.0f;
        }
        bool flagged = false;
        for (Unit &u : m_units)
            if (u.mode == kPatrol) {
                UpdatePatrol(u, world, dt, player);
                flagged = flagged || (u.mode == kPatrol && u.ramCool >= 1.0f);
            }
        m_hud.nearest = 1e9f;
        for (Unit &u : m_units)
            if (u.mode != kOff)
                m_hud.nearest = std::min(m_hud.nearest, std::hypot(u.car.X() - m_px, u.car.Y() - m_py));
        if (flagged || getenv("NFSU2_PURSUIT_NOW")) {
            static bool once = false;
            if (flagged || !once) {
                once = true;
                Start(world, player, getenv("NFSU2_PURSUIT_HEAT") ? atoi(getenv("NFSU2_PURSUIT_HEAT")) : 1);
            }
        }
        return;
    }

    // ---- result screens: the cops stand still
    if (m_state == kBusted || m_state == kEscaped) {
        m_hud.resultTimer += dt;
        for (Unit &u : m_units)
            if (u.mode != kOff) {
                u.car.SetHold(true);
                u.car.Update(world, 0.0f, 0.4f, 0.0f, dt, true, 1.0f);
            }
        return;
    }

    // ---- the chase
    Hud &h = m_hud;
    h.chaseTime += dt;
    h.bounty += dt * (14.0f + 9.0f * float(h.heat));
    m_heatTimer += dt;
    if (m_heatTimer > 42.0f && h.heat < 5) {
        m_heatTimer = 0;
        ++h.heat;
        Banner("HEAT LEVEL " + std::to_string(h.heat), "More units are on their way");
    }
    // reinforcements
    int pursuers = 0, blockers = 0;
    for (Unit &u : m_units) {
        pursuers += u.mode == kPursue;
        blockers += u.mode == kBlock;
    }
    const int want = std::min(int(m_units.size()) - 1, 1 + h.heat + (h.heat >= 3 ? 1 : 0));
    m_spawnTimer -= dt;
    if (pursuers < want && m_spawnTimer <= 0.0f) {
        if (SpawnUnit(world, player, kPursue, 120.0f, 260.0f, true) || SpawnUnit(world, player, kPursue, 100.0f, 420.0f, false) ||
            SpawnUnit(world, player, kPursue, 50.0f, 700.0f, false)) {
            m_spawnTimer = 2.6f;
            m_hadUnits = true;
        } else
            m_spawnTimer = 1.0f;
    }
    m_roadblockTimer -= dt;
    if (h.heat >= 2 && m_roadblockTimer <= 0.0f && blockers == 0 && m_pspeed > 8.0f) {
        MakeRoadblock(world, player);
        m_roadblockTimer = 38.0f - 3.0f * float(h.heat);
    }

    bool seen = false;
    float nearest = 1e9f, bustNear = 1e9f;
    int active = 0;
    for (Unit &u : m_units) {
        if (u.mode == kPatrol) { // a patrol that was not caught up in the start just keeps going
            UpdatePatrol(u, world, dt, player);
            u.ramCool = 0.0f;
        } else if (u.mode == kPursue) {
            UpdatePursuer(u, world, dt, player);
        } else if (u.mode == kBlock) {
            UpdateBlock(u, world, dt, player);
        }
        if (u.mode == kPursue) {
            ++active;
            const float d = std::hypot(u.car.X() - m_px, u.car.Y() - m_py);
            nearest = std::min(nearest, d);
            seen = seen || (u.sees > 0.5f && d < 140.0f);
            bustNear = std::min(bustNear, d);
            // a ram: the cop's contact with the player (the contact itself is resolved by the caller)
            const float rvx = u.car.Speed() * std::cos(u.car.Yaw()) - m_pvx, rvy = u.car.Speed() * std::sin(u.car.Yaw()) - m_pvy;
            if (d < 3.6f && std::hypot(rvx, rvy) > 3.0f && u.ramCool <= 0.0f) {
                u.ramCool = 1.4f;
                ++h.rams;
                h.bounty += 220.0f;
                m_heatTimer += 7.0f;
                h.bust = std::min(1.0f, h.bust + 0.06f);
            }
        } else if (u.mode == kBlock) {
            const float d = std::hypot(u.car.X() - m_px, u.car.Y() - m_py);
            nearest = std::min(nearest, d);
        }
    }
    h.units = active;
    h.nearest = nearest;
    if (getenv("NFSU2_PURSUIT_DEBUG") && int(h.chaseTime) != m_dbgSec) {
        m_dbgSec = int(h.chaseTime);
        printf("[Pursuit] t=%d heat %d cops %d blocks %d nearest %.0f seen %d contact %d cooldown %.2f bust %.2f\n", m_dbgSec, h.heat, active, blockers, nearest, seen,
               m_contact, h.cooldown, h.bust);
        for (const Unit &u : m_units)
            if (u.mode != kOff)
                printf("[Pursuit]    unit %zu mode %d at %.0f %.0f z %.1f speed %.1f route %zu stuck %.1f rev %.1f dist %.0f\n", size_t(&u - m_units.data()), int(u.mode), u.car.X(),
                       u.car.Y(), u.car.Z(), u.car.Speed(), u.route.size(), u.stuck, u.reverse, std::hypot(u.car.X() - m_px, u.car.Y() - m_py));
    }

    // BUSTED: pinned down by a cop at a crawl
    if (bustNear < 7.0f && m_pspeed < 5.5f)
        h.bust += dt / 2.8f;
    else if (bustNear < 11.0f && m_pspeed < 9.0f)
        h.bust += dt / 6.0f;
    else
        h.bust = std::max(0.0f, h.bust - dt * 0.55f);
    if (h.bust >= 1.0f) {
        m_state = kBusted, h.state = kBusted, h.resultTimer = 0;
        h.fine = std::floor((600.0f + h.bounty * 0.18f) / 10.0f) * 10.0f;
        m_resultAmount = h.fine, m_resultBusted = true, m_resultTaken = false;
        printf("[Pursuit] BUSTED (fine %.0f, heat %d, %.0f s)\n", h.fine, h.heat, h.chaseTime);
        return;
    }
    // COOLDOWN: out of their sight
    if (seen)
        m_contact = true;
    if (!seen && !m_contact) {
        // the cops are still on their way: nothing to evade yet
    } else if (!seen && nearest > 110.0f) {
        m_unseen += dt;
        const float rate = 1.0f / std::max(14.0f, 28.0f - 2.2f * float(h.heat)); // 26 s at heat 1 .. 17 s at heat 5
        h.cooldown += dt * rate * (nearest > 260.0f ? 1.8f : 1.0f);
    } else {
        m_unseen = 0.0f;
        h.cooldown = std::max(0.0f, h.cooldown - dt * (seen ? 0.8f : 0.4f));
    }
    h.state = h.cooldown > 0.03f ? kCooldown : kChase;
    if (h.cooldown >= 1.0f || (m_contact && active == 0 && blockers == 0 && h.chaseTime > 12.0f && nearest > 300.0f)) {
        m_state = kEscaped, h.state = kEscaped, h.resultTimer = 0;
        h.reward = std::floor(h.bounty / 10.0f) * 10.0f;
        m_resultAmount = h.reward, m_resultBusted = false, m_resultTaken = false;
        printf("[Pursuit] ESCAPED (bounty %.0f, heat %d, %.0f s)\n", h.reward, h.heat, h.chaseTime);
    }
}
