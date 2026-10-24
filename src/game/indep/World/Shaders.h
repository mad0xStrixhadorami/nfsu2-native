#pragma once
#include <string>
// GLSL 1.20 sources for the world renderer (ISSUES #263). They keep the fixed-function vertex arrays and matrices.
namespace Shaders {

// ---- World: textured, vertex-lit scenery with graded colour and distance fog -------------------------------------
inline const char *kWorldVS = R"(#version 120
varying vec4 vColor;
varying vec2 vUV;
varying float vDist;
varying vec3 vWorld;
varying vec3 vRel;
uniform vec3 uCam;
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    vColor = gl_Color;
    vUV = gl_MultiTexCoord0.xy;
    vWorld = gl_Vertex.xyz;
    vRel = gl_Vertex.xyz - uCam;
    vDist = length((gl_ModelViewMatrix * gl_Vertex).xyz);
}
)";

// Dev aid (NFSU2_SIMPLE_WORLD_FS=1): the cheapest possible world shader (texture x colour, fog), to separate shader cost from draw cost.
inline const char *kWorldFSMin = R"(#version 120
uniform sampler2D uTex;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
varying vec4 vColor;
varying vec2 vUV;
varying float vDist;
void main() {
    vec4 c = texture2D(uTex, vUV) * vColor;
    float f = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
    c.rgb = mix(c.rgb, uFogColor, f);
    gl_FragColor = c;
}
)";

inline const char *kWorldFSBase = R"(#version 120
uniform sampler2D uTex;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uGrade;
uniform vec3 uCam;
uniform float uWet;
uniform float uTheme;   // 1 = Israeli district theme (ISSUES #272)
uniform float uMatOn;   // 1 = photographic materials bound (ISSUES #274)
uniform float uDay;     // 1 = Israeli daylight grade (ISSUES #277)
#ifdef FULL
uniform sampler2D uMC0, uMC1, uMC2, uMC3, uMC4, uMC5, uMC6, uMC7; // asphalt, pavers, cream pavers, stone, cladding, plaster, concrete, stucco
uniform sampler2D uMN0, uMN1, uMN2, uMN3, uMN4;                   // normals: asphalt, pavers, stone, concrete, plaster
#endif
varying vec4 vColor;
varying vec2 vUV;
varying float vDist;
varying vec3 vWorld;
varying vec3 vRel;
float h12(vec2 p) { vec3 q = fract(vec3(p.xyx) * 0.1031); q += dot(q, q.yzx + 33.33); return fract((q.x + q.y) * q.z); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(h12(i), h12(i + vec2(1, 0)), f.x), mix(h12(i + vec2(0, 1)), h12(i + vec2(1, 1)), f.x), f.y);
}
#ifdef FULL
vec3 matC(sampler2D s, vec2 uv) { // two taps at different scales blended by noise break up the repeat
    vec3 a = texture2D(s, uv).rgb, b = texture2D(s, uv * 0.27 + vec2(0.31, 0.17)).rgb;
    return mix(a, b, smoothstep(0.4, 0.6, vnoise(uv * 0.23)) * 0.6);
}
vec3 matN(sampler2D s, vec2 uv) {
    vec3 a = texture2D(s, uv).xyz, b = texture2D(s, uv * 0.27 + vec2(0.31, 0.17)).xyz;
    return normalize(mix(a, b, smoothstep(0.4, 0.6, vnoise(uv * 0.23)) * 0.6) * 2.0 - 1.0);
}
float bumpL(vec3 nts, vec3 T, vec3 B, vec3 N) { // lighting ratio of the perturbed normal against the geometric one
    vec3 L = normalize(vec3(0.35, 0.2, 0.9));
    vec3 wn = normalize(T * nts.x + B * nts.y + N * nts.z);
    return (0.5 + 0.7 * max(dot(wn, L), 0.0)) / (0.5 + 0.7 * max(dot(N, L), 0.0));
}
#endif
void main() {
    vec4 t = texture2D(uTex, vUV);
    vec4 c = t * vColor; // the PS2's MODULATE (vertex colours were doubled on the CPU)
#ifdef FULL
    float wetMask = 0.0;
    if (uWet > 0.0 && vDist < 90.0) {
        // road-like surface: facing up, grey and not bright
        vec3 N = normalize(cross(dFdx(vRel), dFdy(vRel)));
        float mx = max(t.r, max(t.g, t.b)), mn = min(t.r, min(t.g, t.b));
        float road = smoothstep(0.85, 0.97, abs(N.z)) * (1.0 - smoothstep(0.25, 0.5, (mx - mn) / max(mx, 0.01))) * (1.0 - smoothstep(0.45, 0.65, mx)) * t.a;
        float puddles = smoothstep(0.50, 0.66, vnoise(vWorld.xy * 0.11) * 0.65 + vnoise(vWorld.xy * 0.43) * 0.35);
        wetMask = road * mix(0.35, 1.0, puddles) * uWet * (1.0 - smoothstep(55.0, 90.0, vDist));
        c.rgb *= 1.0 - 0.38 * wetMask; // wet asphalt is darker
        vec3 V = normalize(uCam - vWorld);
        float fres = 0.05 + 0.95 * pow(1.0 - clamp(V.z, 0.0, 1.0), 4.0);
        vec3 sky = mix(vec3(0.04, 0.06, 0.10), vec3(0.22, 0.28, 0.40), clamp(1.0 - V.z, 0.0, 1.0));
        // reflected light sources: stretched along the view direction on the ground, warm and cool lamps on a sparse grid
        vec2 g = vWorld.xy / 13.0;
        vec2 cell = floor(g);
        vec2 local = fract(g) - 0.5 - (vec2(h12(cell), h12(cell + 5.0)) - 0.5) * 0.5;
        vec2 away = normalize(V.xy + 1e-4);
        float along = dot(local, away), across = dot(local, vec2(-away.y, away.x));
        float streak = exp(-across * across * 220.0) * exp(-pow(max(along + 0.15, 0.0), 2.0) * 7.0) * step(0.55, h12(cell + 9.0));
        vec3 lamp = mix(vec3(1.0, 0.62, 0.25), vec3(0.35, 0.6, 1.0), step(0.7, h12(cell + 2.0)));
        c.rgb += wetMask * (sky * fres * 1.2 + lamp * streak * (0.35 + 0.65 * fres) * 0.9);
    }

    if (uTheme > 0.0) {
        // ---- Israeli district theme: four cities blended by distance from their centres in the map ----
        // x = Tel Aviv (Coastal Commons), y = Jerusalem (Upper City), z = Haifa (Skyline Heights), w = Be'er Sheva (Industrial).
        vec4 wd = vec4(exp(-dot(vWorld.xy - vec2(-300.0, -420.0), vWorld.xy - vec2(-300.0, -420.0)) / 640000.0),
                       exp(-dot(vWorld.xy - vec2(-1050.0, 580.0), vWorld.xy - vec2(-1050.0, 580.0)) / 490000.0),
                       exp(-dot(vWorld.xy - vec2(-1850.0, 1120.0), vWorld.xy - vec2(-1850.0, 1120.0)) / 490000.0),
                       exp(-dot(vWorld.xy - vec2(-250.0, -1650.0), vWorld.xy - vec2(-250.0, -1650.0)) / 640000.0));
        wd /= max(dot(wd, vec4(1.0)), 1e-4);
        vec3 Nt = normalize(cross(dFdx(vRel), dFdy(vRel)));
        float lumT = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        float satT = (max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b))) / max(max(c.r, max(c.g, c.b)), 0.01);
        float fl = vColor.a * 255.0;                       // triangle flags set by IsraelProps: 254 prop, 253 kerb face, 252 bike-lane pavement
        float isProp = step(253.5, fl) * step(fl, 254.5);
        float isKerb = step(252.5, fl) * step(fl, 253.5);
        float isBike = step(251.5, fl) * step(fl, 252.5);
        float wall = (1.0 - smoothstep(0.25, 0.5, abs(Nt.z))) * t.a * (1.0 - isProp);
        float flatUp = smoothstep(0.85, 0.97, Nt.z) * t.a;
        // facade courses along the wall (u runs horizontally along it)
        float along = dot(vWorld.xy, normalize(vec2(-Nt.y, Nt.x) + 1e-5));
        float keep = smoothstep(0.07, 0.16, lumT) * (1.0 - smoothstep(0.35, 0.7, satT)); // windows / signs stay as they were
        // Jerusalem stone: pale limestone ashlar, rows 0.55 m high, blocks 1.1 m wide, brick-bonded
        float row = vWorld.z / 0.55, rowI = floor(row);
        vec2 bu = vec2(along / 1.1 + 0.5 * mod(rowI, 2.0), row);
        vec2 bf = fract(bu);
        float mortar = 1.0 - smoothstep(0.0, 0.05, min(min(bf.x, 1.0 - bf.x) * 0.5, min(bf.y, 1.0 - bf.y) * 0.3));
        float grain = 0.88 + 0.22 * vnoise(vec2(along * 3.0, vWorld.z * 3.0) + floor(bu) * 7.0);
        vec3 stone = vec3(0.86, 0.79, 0.64) * grain * (1.0 - 0.22 * mortar) * (0.55 + 0.7 * lumT);
        // Tel Aviv: white Bauhaus plaster with a faint horizontal band
        vec3 plaster = vec3(0.93, 0.92, 0.88) * (0.62 + 0.55 * lumT) * (1.0 - 0.06 * step(0.92, fract(vWorld.z / 3.0)));
        // Be'er Sheva: board-marked brutalist concrete, sun bleached
        float board = 0.93 + 0.07 * vnoise(vec2(along * 0.6, vWorld.z * 7.0));
        vec3 concrete = vec3(0.64, 0.62, 0.58) * board * (0.6 + 0.6 * lumT) * (1.0 - 0.12 * step(0.96, fract(vWorld.z / 3.2)));
        // Haifa: terraced houses in warm stucco on the slopes
        vec3 stucco = vec3(0.90, 0.80, 0.66) * (0.6 + 0.6 * lumT) * (0.94 + 0.06 * vnoise(vWorld.xy * 0.4));
        vec3 skin = stone * wd.y + plaster * wd.x + concrete * wd.w + stucco * wd.z;
        c.rgb = mix(c.rgb, skin * vColor.rgb * 1.15, wall * keep * uTheme * 0.92 * (1.0 - uMatOn));
        // gray interlocking paving stones on bright flat ground (pavements, plazas), 0.22 x 0.11 m pavers in herringbone-ish rows
        float pave = flatUp * (1.0 - isProp) * smoothstep(0.42, 0.55, lumT) * (1.0 - smoothstep(0.2, 0.4, satT));
        vec2 pp = vWorld.xy / vec2(0.22, 0.11);
        float prow = floor(pp.y);
        vec2 pf = fract(vec2(pp.x + 0.5 * mod(prow, 2.0), pp.y));
        float joint = 1.0 - smoothstep(0.0, 0.08, min(min(pf.x, 1.0 - pf.x), min(pf.y, 1.0 - pf.y)) * 2.0);
        float tile = 0.92 + 0.12 * h12(vec2(floor(pp.x + 0.5 * mod(prow, 2.0)), prow));
        c.rgb = mix(c.rgb, c.rgb * tile * (1.0 - 0.28 * joint), pave * uTheme * (1.0 - uMatOn) * (1.0 - smoothstep(25.0, 60.0, vDist)));
        if (uMatOn > 0.5) {
            // ---- photographic materials (ISSUES #274), mapped by world position; classification from the unlit texture ----
            float mxO = max(t.r, max(t.g, t.b)), mnO = min(t.r, min(t.g, t.b));
            float satO = (mxO - mnO) / max(mxO, 0.01);
            float nearM = 1.0 - smoothstep(70.0, 170.0, vDist);
            vec3 up = Nt;
            vec3 Tg = vec3(1.0, 0.0, 0.0), Bg = vec3(0.0, 1.0, 0.0);
            // asphalt: dark desaturated flat surfaces that are not lane paint
            float roadM = flatUp * (1.0 - isProp) * (1.0 - smoothstep(0.22, 0.45, satO)) * (1.0 - smoothstep(0.42, 0.62, mxO)) * (1.0 - isBike);
            if (roadM * nearM > 0.01) {
                vec2 ru = vWorld.xy / 3.4;
                vec3 col = matC(uMC0, ru) * vec3(0.80, 0.79, 0.78);
                col *= mix(1.0, clamp(mxO / 0.25, 0.55, 1.3), 0.45);       // keep the original's tyre marks and patches
                float bl = mix(1.0, bumpL(matN(uMN0, ru), Tg, Bg, up), 0.8);
                c.rgb = mix(c.rgb, col * vColor.rgb * 1.15 * bl, roadM * nearM * 0.92);
            }
            // pavements and plazas: grey block pavers, cream paving in Jerusalem and on the Tel Aviv promenade
            float paveM = flatUp * (1.0 - isProp) * (1.0 - roadM) * smoothstep(0.40, 0.56, mxO) * (1.0 - smoothstep(0.25, 0.5, satO));
            if (paveM * nearM > 0.01) {
                vec2 pu = vWorld.xy / 2.6;
                float cream = clamp(wd.y * 1.2 + wd.x * 0.35, 0.0, 1.0);
                vec3 grey = matC(uMC1, pu), warm = matC(uMC2, vWorld.xy / 2.2);
                vec3 col = mix(grey, warm, cream);
                float bl = mix(1.0, bumpL(matN(uMN1, pu), Tg, Bg, up), 0.7 * (1.0 - cream));
                c.rgb = mix(c.rgb, col * vColor.rgb * 1.2 * bl, paveM * nearM * 0.95);
            }
            // facades
            if (wall * keep * nearM > 0.01) {
                vec3 Tw = normalize(cross(vec3(0.0, 0.0, 1.0), Nt) + vec3(1e-4, 0.0, 0.0));
                vec3 Bw = cross(Nt, Tw);
                vec2 uv = vec2(dot(vWorld, Tw), vWorld.z);
                vec3 skinM = vec3(0.0), nM = vec3(0.0);
                if (wd.y > 0.03) { // Jerusalem stone; patches of finer cladding
                    float patch = step(0.68, h12(floor(vWorld.xy / 55.0) + 11.0));
                    vec3 st = mix(matC(uMC3, uv / 2.6), matC(uMC4, uv / 2.4), patch) * vec3(1.04, 1.0, 0.95);
                    skinM += st * wd.y, nM += matN(uMN2, uv / 2.6) * wd.y;
                }
                if (wd.x > 0.03) { // Tel Aviv white render
                    skinM += matC(uMC5, uv / 3.2) * vec3(1.0, 0.99, 0.96) * wd.x, nM += matN(uMN4, uv / 3.2) * wd.x;
                }
                if (wd.w > 0.03) { // Be'er Sheva board-marked concrete
                    skinM += matC(uMC6, uv / 3.6) * vec3(1.02, 0.99, 0.92) * wd.w, nM += matN(uMN3, uv / 3.6) * wd.w;
                }
                if (wd.z > 0.03) { // Haifa stucco in warm tones
                    skinM += matC(uMC7, uv / 3.0) * vec3(1.10, 0.97, 0.82) * wd.z, nM += vec3(0.0, 0.0, 1.0) * wd.z;
                }
                float bl = mix(1.0, bumpL(normalize(nM + 1e-4), Tw, Bw, Nt), 0.85);
                c.rgb = mix(c.rgb, skinM * vColor.rgb * 1.2 * bl, wall * keep * nearM * uTheme * 0.95);
            }
        }
        // painted kerbs: 1 m stripes, red-white (no parking) or blue-white (paid parking) by 48 m stretch of street
        {
            float seg = floor(along / 1.0);
            float stripe = mod(seg, 2.0);
            float blueZone = step(0.5, h12(floor(vWorld.xy / 48.0) + 3.0));
            vec3 paint = stripe < 0.5 ? vec3(0.95, 0.95, 0.92) : mix(vec3(0.82, 0.08, 0.08), vec3(0.08, 0.28, 0.80), blueZone);
            c.rgb = mix(c.rgb, paint * (0.75 + 0.45 * vColor.r), isKerb * 0.95);
        }
        // green bike lanes on the Tel Aviv pavement strips, with a white dashed edge
        {
            float dash = step(0.5, fract(dot(vWorld.xy, vec2(0.7071)) / 2.0));
            vec3 lane = vec3(0.16, 0.55, 0.30) * (0.6 + 0.7 * lumT);
            c.rgb = mix(c.rgb, lane, isBike * 0.85);
        }
        // sun-bleached desert light in Be'er Sheva, cooler marine air in Tel Aviv, green haze on the Haifa slopes
        float lT = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        c.rgb = mix(c.rgb, vec3(lT), 0.18 * wd.w * uTheme);
        c.rgb = mix(c.rgb, c.rgb * vec3(1.12, 1.04, 0.86) + 0.05, wd.w * uTheme);
        c.rgb = mix(c.rgb, c.rgb * vec3(0.96, 1.01, 1.06) + 0.02, wd.x * uTheme);
        c.rgb = mix(c.rgb, c.rgb * vec3(0.95, 1.05, 0.93), wd.z * 0.6 * uTheme);
    }
#endif // FULL
    // daylight: the baked lighting is a dusk city; lift it towards a hard Mediterranean sun, warm and a little bleached
    {   // a tone curve, not a gain: shadows lift 2.3x, the highlights stay below white (no blown walls)
        vec3 x = c.rgb * vec3(1.65, 1.58, 1.45);
        c.rgb = mix(c.rgb, x / (1.0 + x) * 1.28, uDay);
    }
    c.rgb = mix(c.rgb, vec3(dot(c.rgb, vec3(0.333))), 0.10 * uDay);
    // grade: a little saturation and a gentle shadow lift so the baked lighting does not crush to black
    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    c.rgb = mix(vec3(l), c.rgb, 1.0 + 0.16 * uGrade);
    c.rgb += (1.0 - l) * 0.018 * uGrade * vec3(0.55, 0.7, 1.0);
    // distance fog towards the horizon colour (smooth, then a slow exponential tail)
    float f = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
    f = f * f * (3.0 - 2.0 * f);
    c.rgb = mix(c.rgb, uFogColor, f);
    gl_FragColor = c;
}
)";

// The world fragment shader comes in two variants (ISSUES #282): FULL (district theme, photographic materials, wet roads) for the
// cells near the camera, and a lite one (colour, daylight curve, fog) for everything far away.  One big shader with runtime
// branches made every pixel pay for the registers of the whole thing (35 ms vs 13 ms per frame on an integrated GPU).
inline std::string WorldFSSource(bool full)
{
    std::string src = kWorldFSBase;
    if (full)
        src.insert(src.find('\n') + 1, "#define FULL 1\n");
    return src;
}



// ---- Office windows: interior mapping (ISSUES #266). The window texels of tall facades are replaced by a procedural
// room found with a ray through a box grid (Van Dongen's interior mapping): back and side walls, ceiling light panel, floor,
// desks, monitors and chairs, each room lit warm, lit cool or dark by a hash of its cell, with a glass reflection on top. ----
inline const char *kInteriorVS = R"(#version 120
varying vec4 vColor;
varying vec2 vUV;
varying float vDist;
varying vec3 vWorld;
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    vColor = gl_Color;
    vUV = gl_MultiTexCoord0.xy;
    vWorld = gl_Vertex.xyz;
    vDist = length((gl_ModelViewMatrix * gl_Vertex).xyz);
}
)";

inline const char *kInteriorFS = R"(#version 120
uniform sampler2D uTex;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uGrade;
uniform vec3 uCam;
uniform float uTime;
uniform float uLit;      // fraction of rooms with the lights on
uniform float uGlassOnly; // 1 = the whole texture is glass (curtain walls), 0 = detect windows in the texture
uniform float uDebug;
varying vec4 vColor;
varying vec2 vUV;
varying float vDist;
varying vec3 vWorld;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec2 hash22(vec2 p) { return vec2(hash12(p), hash12(p + 17.3)); }

// ray / axis aligned box; returns the near hit distance (or -1) and the face normal
float boxHit(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax, out vec3 n) {
    vec3 inv = 1.0 / rd;
    vec3 t0 = (bmin - ro) * inv, t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1), tmax = max(t0, t1);
    float tn = max(max(tmin.x, tmin.y), tmin.z), tf = min(min(tmax.x, tmax.y), tmax.z);
    if (tf < max(tn, 0.0)) return -1.0;
    n = tn == tmin.x ? vec3(-sign(rd.x), 0.0, 0.0) : (tn == tmin.y ? vec3(0.0, -sign(rd.y), 0.0) : vec3(0.0, 0.0, -sign(rd.z)));
    return tn;
}

// A room in its own frame: a along the facade, b up, c into the building. ro is the ray start on the window plane.
vec3 room(vec3 ro, vec3 rd, vec2 cell, vec3 size, float detail) {
    float h0 = hash12(cell), h1 = hash12(cell + 3.1), h2 = hash12(cell + 7.7), h3 = hash12(cell + 11.9);
    bool lit = h0 < uLit;
    bool cool = h1 < 0.3;
    vec3 lightCol = cool ? vec3(0.78, 0.90, 1.0) : mix(vec3(1.0, 0.82, 0.55), vec3(1.0, 0.95, 0.80), h2);
    float power = lit ? mix(0.7, 1.35, h3) : 0.0;
    // the room's shell
    float tx = rd.x > 0.0 ? (size.x - ro.x) / rd.x : -ro.x / rd.x;
    float ty = rd.y > 0.0 ? (size.y - ro.y) / rd.y : -ro.y / rd.y;
    float tz = (size.z - ro.z) / rd.z;
    float t = min(min(tx, ty), tz);
    vec3 hit = ro + rd * t;
    vec3 wall = mix(vec3(0.55, 0.53, 0.50), vec3(0.45, 0.50, 0.58), h1) * (0.7 + 0.5 * h2);
    vec3 albedo = wall;
    vec3 nrm;
    if (t == tz) { // back wall: a panel, a poster or shelves
        nrm = vec3(0.0, 0.0, -1.0);
        vec2 q = hit.xy / size.xy;
        float poster = step(0.58, h3) * step(abs(q.x - 0.5 - (h0 - 0.5) * 0.5), 0.14) * step(abs(q.y - 0.62), 0.16);
        albedo = mix(wall, mix(vec3(0.8, 0.2, 0.15), vec3(0.15, 0.4, 0.8), h1), poster * 0.85);
        albedo *= 0.8 + 0.2 * smoothstep(0.0, 0.1, q.y); // skirting
    } else if (t == ty) {
        nrm = vec3(0.0, rd.y > 0.0 ? -1.0 : 1.0, 0.0);
        albedo = rd.y > 0.0 ? vec3(0.8) : mix(vec3(0.16, 0.17, 0.22), vec3(0.28, 0.2, 0.16), h1); // ceiling tiles / carpet
        if (rd.y > 0.0) { // a ceiling light panel (emissive when the lights are on)
            vec2 q = (hit.xz - size.xz * 0.5) / size.xz;
            float panel = step(abs(q.x), 0.28) * step(abs(q.y), 0.12);
            albedo = mix(albedo, lightCol * 2.2 * power, panel);
        }
    } else {
        nrm = vec3(rd.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
        albedo = wall * 0.85;
    }
    // furniture: a desk with a monitor and a chair, or a shelf unit, placed by the room hash
    vec3 deskMin = vec3(size.x * (0.12 + 0.2 * h0), 0.0, size.z * (0.30 + 0.25 * h2));
    vec3 deskMax = vec3(deskMin.x + size.x * (0.38 + 0.25 * h1), 0.74, deskMin.z + size.z * 0.18);
    vec3 bn;
    float bt = boxHit(ro, rd, deskMin, deskMax, bn);
    vec3 col = albedo;
    float emissive = 0.0;
    if (detail > 0.5 && h3 > 0.12 && bt > 0.0 && bt < t) {
        t = bt; nrm = bn; albedo = mix(vec3(0.30, 0.22, 0.15), vec3(0.55, 0.55, 0.58), h1);
        col = albedo;
    }
    // monitor on the desk (faces the window): a thin bright box
    vec3 monMin = vec3(deskMin.x + (deskMax.x - deskMin.x) * 0.35, 0.74, deskMin.z + 0.04);
    vec3 monMax = vec3(monMin.x + 0.62, 1.2, monMin.z + 0.06);
    float mt = boxHit(ro, rd, monMin, monMax, bn);
    if (detail > 0.5 && h3 > 0.12 && mt > 0.0 && mt < t) {
        t = mt; nrm = bn; albedo = vec3(0.04);
        // the screen faces the window (-c): bright when someone works late
        if (bn.z < -0.5 && (lit || h0 > 0.45)) {
            emissive = 1.0;
            albedo = mix(vec3(0.25, 0.55, 1.0), vec3(0.2, 0.9, 0.5), h2) * (0.9 + 0.1 * sin(uTime * 3.0 + h0 * 20.0));
        }
    }
    // a chair in front of the desk
    vec3 chMin = vec3(deskMin.x + (deskMax.x - deskMin.x) * 0.4, 0.0, deskMax.z + 0.1);
    vec3 chMax = chMin + vec3(0.5, 0.95, 0.5);
    float ct = boxHit(ro, rd, chMin, chMax, bn);
    if (detail > 0.5 && h3 > 0.3 && ct > 0.0 && ct < t) {
        t = ct; nrm = bn; albedo = vec3(0.08, 0.09, 0.12);
    }
    // lighting: the ceiling panel (or a monitor glow / TV flicker in a dark room) plus a little ambient
    vec3 P = ro + rd * t;
    vec3 lp = vec3(size.x * 0.5, size.y - 0.05, size.z * 0.5);
    vec3 toL = lp - P;
    float dist = length(toL);
    float diff = max(dot(nrm, toL / dist), 0.0);
    vec3 light = lightCol * power * (0.35 + 0.95 * diff) / (1.0 + 0.045 * dist * dist);
    vec3 glow = vec3(0.0);
    if (!lit) {
        float tv = step(0.7, hash12(cell + 91.0));
        vec3 tvCol = vec3(0.25, 0.4, 0.8) * (0.75 + 0.25 * sin(uTime * 7.0 + h0 * 30.0 + sin(uTime * 2.1) * 3.0));
        glow = tvCol * tv * (0.10 + 0.14 * diff) + vec3(0.012, 0.014, 0.02);
    }
    // dark rooms still catch the city's glow through the glass (orange street light from below, blue sky from above)
    vec3 cityGlow = mix(vec3(0.045, 0.036, 0.028), vec3(0.024, 0.032, 0.05), clamp(P.y / size.y, 0.0, 1.0));
    col = albedo * (light + glow + 0.06 * power + cityGlow * (1.0 + 0.5 * h1));
    col = mix(col, albedo, emissive); // emissive surfaces show their own colour
    return col;
}

void main() {
    vec4 tex = texture2D(uTex, vUV);
    vec4 facade = tex * vColor;
    vec3 dpx = dFdx(vWorld), dpy = dFdy(vWorld);
    vec3 N = normalize(cross(dpx, dpy));
    vec3 V = vWorld - uCam;
    // the normal must face the camera, and the wall must be vertical
    if (dot(N, V) > 0.0) N = -N;
    float fade = 1.0 - smoothstep(110.0, 260.0, vDist);
    float luma = dot(tex.rgb, vec3(0.299, 0.587, 0.114));
    float mx = max(tex.r, max(tex.g, tex.b)), mn = min(tex.r, min(tex.g, tex.b));
    float sat = (mx - mn) / max(mx, 0.001);
    // window texels: dark, grey-blue glass, or bright warm lit panes (the night versions of the facades)
    float dark = (1.0 - smoothstep(0.30, 0.52, luma)) * (1.0 - smoothstep(0.42, 0.78, sat));
    float lit = smoothstep(0.52, 0.72, luma) * smoothstep(0.10, 0.35, tex.r - tex.b);
    float mask = mix(max(dark, lit), 1.0, uGlassOnly);
    mask *= step(abs(N.z), 0.35) * fade * tex.a;
    vec3 c = facade.rgb;
    if (mask > 0.01) {
        vec3 up = vec3(0.0, 0.0, 1.0);
        vec3 right = normalize(cross(up, N));
        vec3 d = normalize(V);
        // room frame: a = along the facade, b = up, c = into the building (0 on the window plane)
        const vec3 size = vec3(3.6, 3.1, 4.6);
        float a = dot(vWorld, right), b = vWorld.z;
        vec2 ij = vec2(floor(a / size.x), floor(b / size.y));
        vec2 cell = ij + floor(dot(vWorld, N) * 0.37) * 31.0; // the hash seed (differs between facades)
        vec3 ro = vec3(a - ij.x * size.x, b - ij.y * size.y, 0.0);
        vec3 rd = vec3(dot(d, right), d.z, -dot(d, N));
        if (rd.z > 0.02) {
            vec3 inside = room(ro, rd, cell, size, 1.0 - smoothstep(55.0, 95.0, vDist) > 0.5 ? 1.0 : 0.0);
            // a little glass: sky / street reflection that grows at grazing angles, and a faint tint
            float cosI = clamp(rd.z, 0.0, 1.0);
            float F = 0.06 + 0.94 * pow(1.0 - cosI, 4.0);
            vec3 reflection = mix(vec3(0.05, 0.07, 0.11), vec3(0.20, 0.25, 0.34), clamp(0.5 + 0.5 * reflect(d, N).z, 0.0, 1.0));
            vec3 glass = inside * (1.0 - F) * vec3(0.95, 1.0, 1.0) + reflection * F;
            c = mix(c, glass, clamp(mask, 0.0, 0.92));
        }
    }
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, 1.0 + 0.16 * uGrade);
    float f = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
    f = f * f * (3.0 - 2.0 * f);
    c = mix(c, uFogColor, f);
    if (uDebug > 1.5) c = vec3(mask, fade, step(abs(N.z), 0.35));
    else if (uDebug > 0.5) c = mix(c, vec3(1.0, 0.0, 1.0) * (0.4 + 0.6 * mask), 0.45);
    gl_FragColor = vec4(c, facade.a);
}
)";

// ---- Cars: per-pixel paint with a clear coat, a procedural night-city environment and Fresnel -------------------------------
inline const char *kCarVS = R"(#version 120
attribute vec3 aNormal;
attribute float aGloss;
attribute float aAO;
varying float vAO;
varying vec4 vColor;
varying vec2 vUV;
varying vec3 vNormal;
varying vec3 vPos;
varying float vGloss;
varying float vDist;
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    vColor = gl_Color;
    vUV = gl_MultiTexCoord0.xy;
    vNormal = aNormal;
    vPos = gl_Vertex.xyz;
    vGloss = aGloss;
    vAO = aAO;
    vDist = length((gl_ModelViewMatrix * gl_Vertex).xyz);
}
)";

inline const char *kCarFS = R"(#version 120
uniform sampler2D uTex;
uniform vec3 uCam;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uTime;
uniform float uBlur;
varying float vAO;
varying vec4 vColor;
varying vec2 vUV;
varying vec3 vNormal;
varying vec3 vPos;
varying float vGloss;
varying float vDist;

// A procedural environment: night sky with a sodium horizon glow, rows of distant city lights and a couple of soft
// light boxes, so the clear coat has something to reflect (the original uses an environment-map texture).
vec3 environment(vec3 R) {
    float h = R.z;
    vec3 zenith = vec3(0.03, 0.05, 0.13), horizon = vec3(0.30, 0.27, 0.30), ground = vec3(0.045, 0.045, 0.055);
    vec3 col = mix(ground, mix(horizon, zenith, clamp(h * 1.6, 0.0, 1.0)), smoothstep(-0.12, 0.08, h));
    col += vec3(0.75, 0.45, 0.22) * exp(-abs(h - 0.02) * 12.0) * 0.55;               // sodium glow on the horizon
    float a = atan(R.y, R.x);
    float lights = pow(max(0.0, sin(a * 41.0 + 0.7)), 36.0) * pow(max(0.0, sin(a * 13.0)), 4.0);
    col += vec3(1.0, 0.85, 0.55) * lights * exp(-abs(h - 0.07) * 34.0) * 1.3;        // window / street lights
    col += vec3(0.55, 0.72, 1.0) * pow(max(0.0, dot(R, normalize(vec3(0.35, 0.45, 0.82)))), 70.0) * 1.35; // key light box
    col += vec3(1.0, 0.8, 0.6) * pow(max(0.0, dot(R, normalize(vec3(-0.6, -0.2, 0.77)))), 60.0) * 1.2;   // fill
    return col;
}

void main() {
    vec4 tex = texture2D(uTex, vUV);
    vec4 base = tex * vColor;
    if (vGloss < 0.5) {
        // tyres, lights, badges: the CPU-lit vertex colour
        float f0 = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
        gl_FragColor = vec4(mix(base.rgb, uFogColor, f0 * f0 * (3.0 - 2.0 * f0)), base.a);
        return;
    }
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCam - vPos);
    vec3 L = normalize(vec3(0.40, 0.30, 0.866));
    if (vGloss > 2.5) {
        // ---- wheels (ISSUES #264): 3 alloy rim, 4 rubber tyre, 5 brake disc ----------------------------------------------
        float NdV = max(dot(N, V), 0.0);
        float NdL = max(dot(N, L), 0.0);
        vec3 H = normalize(L + V);
        float NdH = max(dot(N, H), 0.0);
        vec3 R = reflect(-V, N);
        vec3 env = environment(R);
        vec3 col;
        if (vGloss < 3.5) {
            // alloy: metal reflection tinted by the rim paint, bevel highlights, occlusion deep in the barrel
            vec3 paint = clamp(vColor.rgb, 0.0, 1.0);
            vec3 F0 = mix(vec3(0.62), paint * 0.8 + 0.2, 0.5);
            vec3 F = F0 + (1.0 - F0) * pow(1.0 - NdV, 5.0);
            float diffuse = 0.22 + 0.40 * NdL;
            col = paint * diffuse * 0.9 + env * F * 2.1 + F0 * (pow(NdH, 90.0) * 1.6 + pow(NdH, 14.0) * 0.25) * (0.35 + 0.65 * NdL);
            col *= vAO;
            // spoke blur: at speed the spokes fade into a translucent disc of the average rim tone
            vec3 disc = (paint * 0.35 + env * F0 * 0.35) * (0.55 + 0.45 * vAO);
            col = mix(col, disc, uBlur * 0.8);
        } else if (vGloss < 4.5) {
            // rubber: near-black with the sidewall print kept, a broad grazing sheen and a faint tread glint
            vec3 print = tex.rgb * vColor.rgb;
            float luma = dot(print, vec3(0.299, 0.587, 0.114));
            vec3 rubber = vec3(0.022, 0.022, 0.025) + vec3(luma) * 0.42;
            float sheen = pow(1.0 - NdV, 3.0) * 0.14 + pow(NdH, 24.0) * 0.07;
            col = rubber * (0.55 + 0.55 * NdL) + env * (0.03 + sheen) + vec3(sheen * 0.4);
            col *= mix(1.0, vAO, 0.6);
        } else {
            // brake disc: brushed grey steel, darker inside the wheel
            vec3 steel = vec3(0.34, 0.34, 0.36);
            float groove = 0.9 + 0.1 * sin(length(vPos.xz) * 520.0);
            vec3 F = vec3(0.5) + 0.5 * pow(1.0 - NdV, 5.0);
            col = steel * (0.25 + 0.4 * NdL) * groove + env * F * 0.55 + vec3(pow(NdH, 40.0) * 0.5);
            col *= 0.55 + 0.45 * vAO;
        }
        float fw = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
        gl_FragColor = vec4(mix(col, uFogColor, fw * fw * (3.0 - 2.0 * fw)), (vGloss < 3.5 || vGloss > 4.5) ? 1.0 : tex.a); // the tyre keeps its alpha-tested corners
        return;
    }
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    bool glass = vGloss > 1.5;
    // base lighting: hemisphere + sun, with a wrap term so the shadow side stays rich
    float hemi = 0.26 + 0.30 * (0.5 + 0.5 * N.z);
    float wrap = clamp((dot(N, L) + 0.35) / 1.35, 0.0, 1.0);
    vec3 lit = base.rgb * (hemi + 0.62 * wrap);
    // clear coat: Fresnel-weighted environment reflection and two specular lobes (tight sun + broad sheen)
    vec3 R = reflect(-V, N);
    float F0 = glass ? 0.085 : 0.045;
    float fres = F0 + (1.0 - F0) * pow(1.0 - NdV, 5.0);
    float reflectivity = glass ? 1.0 : 0.85;
    vec3 env = environment(R);
    vec3 H = normalize(L + V);
    float NdH = max(dot(N, H), 0.0);
    float spec = pow(NdH, glass ? 260.0 : 140.0) * 1.1 + pow(NdH, 18.0) * 0.12;
    // a metallic-flake glint that moves with the view (paint only)
    float flake = glass ? 0.0 : pow(max(0.0, sin(dot(vPos, vec3(61.0, 47.0, 53.0)) + NdV * 7.0)), 40.0) * 0.06 * (0.3 + NdL);
    vec3 col = lit * (1.0 - fres * reflectivity * 0.65) + env * fres * reflectivity + vec3(spec) * (0.4 + 0.6 * NdL) + base.rgb * flake;
    if (glass) col = col * 0.55 + env * 0.18; // tinted glass picks up some of the surroundings even head-on
    float f = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
    col = mix(col, uFogColor, f * f * (3.0 - 2.0 * f));
    gl_FragColor = vec4(col, base.a);
}
)";

// ---- Post-processing: bright pass (4x down), separable blur, composite with tone curve, bloom and vignette -------------
inline const char *kPostVS = R"(#version 120
varying vec2 vUV;
void main() {
    gl_Position = gl_Vertex;
    vUV = gl_MultiTexCoord0.xy;
}
)";

inline const char *kBrightFS = R"(#version 120
uniform sampler2D uScene;
uniform vec2 uTexel;
uniform float uThreshold;
varying vec2 vUV;
void main() {
    vec3 c = vec3(0.0);
    for (int y = -1; y <= 2; ++y)
        for (int x = -1; x <= 2; ++x) {
            vec3 s = texture2D(uScene, vUV + (vec2(float(x), float(y)) - 0.5) * uTexel).rgb;
            float l = dot(s, vec3(0.299, 0.587, 0.114));
            c += s * smoothstep(uThreshold, uThreshold + 0.25, l); // soft knee
        }
    gl_FragColor = vec4(c / 16.0, 1.0);
}
)";

inline const char *kBlurFS = R"(#version 120
uniform sampler2D uSrc;
uniform vec2 uDir; // texel step, one axis
varying vec2 vUV;
void main() {
    vec3 c = texture2D(uSrc, vUV).rgb * 0.2270270270;
    c += (texture2D(uSrc, vUV + uDir * 1.3846153846).rgb + texture2D(uSrc, vUV - uDir * 1.3846153846).rgb) * 0.3162162162;
    c += (texture2D(uSrc, vUV + uDir * 3.2307692308).rgb + texture2D(uSrc, vUV - uDir * 3.2307692308).rgb) * 0.0702702703;
    gl_FragColor = vec4(c, 1.0);
}
)";

inline const char *kCompositeFS = R"(#version 120
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uBloomStrength;
uniform float uVignette;
uniform float uExposure;
varying vec2 vUV;
vec3 filmic(vec3 x) { // ACES fit
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
void main() {
    vec3 c = texture2D(uScene, vUV).rgb;
    vec3 b = texture2D(uBloom, vUV).rgb;
    c += b * uBloomStrength;
    c *= uExposure;
    c = mix(c, filmic(c), 0.55);                       // a soft filmic shoulder on top of the display curve
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, 1.08);                         // slight saturation
    vec2 d = vUV - 0.5;
    c *= 1.0 - uVignette * smoothstep(0.18, 0.62, length(d * vec2(1.0, 0.85)));
    gl_FragColor = vec4(c, 1.0);
}
)";
} // namespace Shaders
