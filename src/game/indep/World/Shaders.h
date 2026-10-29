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
uniform float uFoliage; // 1 while leaves are drawn (ISSUES #314)
uniform float uTime;
void main() {
    vec4 p = gl_Vertex;
    if (uFoliage > 0.5) {
        // wind: a slow, spatially coherent gust that bends whole crowns plus a quick flutter of the leaf cards (coherent over a few metres, so
        // neighbouring cards of one tree move together and no gaps open)
        // a travelling gust front rolls across the street; each crown leans with it and springs back, leaf tips whip and rustle more than
        // the card's base (weight from the texture row: top of the card = V 0)
        float tw = clamp(1.0 - fract(gl_MultiTexCoord0.y + 0.0001), 0.0, 1.0);
        float front = dot(p.xy, vec2(0.82, 0.57)) * 0.045 - uTime * 0.9;
        float gust = pow(0.5 + 0.5 * sin(front) * (0.7 + 0.3 * sin(front * 0.31 + 1.7)), 1.6);
        float phase = dot(p.xy, vec2(0.37, 0.23));
        vec2 lean = vec2(0.82, 0.57) * gust * 0.55;
        vec2 sway = vec2(sin(uTime * 1.3 + phase), cos(uTime * 1.1 + phase * 1.3)) * 0.10;
        vec2 rustle = vec2(sin(uTime * 6.1 + dot(p.xyz, vec3(2.9, 3.7, 4.3))), cos(uTime * 5.3 + dot(p.xyz, vec3(3.3, 2.1, 5.1)))) * (0.035 + 0.07 * gust);
        p.xy += (lean + sway) * (0.45 + 0.55 * tw) + rustle * tw;
        p.z -= gust * 0.12 * tw;
    }
    gl_Position = gl_ModelViewProjectionMatrix * p;
    vColor = gl_Color;
    vUV = gl_MultiTexCoord0.xy;
    vWorld = gl_Vertex.xyz;
    vRel = gl_Vertex.xyz - uCam;
    vDist = length((gl_ModelViewMatrix * p).xyz);
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
uniform vec3 uSpotPos0, uSpotPos1, uSpotDir; // the player's headlights lighting the street (ISSUES #312)
uniform float uSpotOn;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uGrade;
uniform vec3 uCam;
uniform vec3 uSun;      // towards the sun (time of day, ISSUES #333)
uniform float uWet;
uniform float uTheme;   // 1 = Israeli district theme (ISSUES #272)
uniform float uMatOn;   // 1 = photographic materials bound (ISSUES #274)
uniform float uDay;     // 1 = Israeli daylight grade (ISSUES #277); continuous with the time of day (#333)
uniform float uSunset;  // 0 .. 1 while the sun is near the horizon: warms the whole grade (#333)
uniform float uDebugPave;
uniform float uFoliage; // 1 while leaves are drawn (ISSUES #314)
uniform float uTime;
uniform float uGlow;    // multiplier for additive (light) textures: the night-time light pools wash the road out in daylight
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
// Worn Israeli interlocking paving ("Eckerstein", ISSUES #300): 0.20 x 0.10 m blocks in a running bond turned off the world axes, each block
// its own tan / reddish-brown / faded tone, edges rubbed pale, joints filled with sand and dirt, grime in broad patches, scattered dry
// leaves and the odd bit of litter.
vec3 paverTex(vec2 w) {
    vec2 q = vec2(0.88 * w.x + 0.475 * w.y, -0.475 * w.x + 0.88 * w.y);
    vec2 p = q / vec2(0.20, 0.10);
    float row = floor(p.y);
    p.x += 0.5 * mod(row, 2.0);
    vec2 id = floor(p), f = fract(p);
    float jd = min(min(f.x, 1.0 - f.x) * 0.20, min(f.y, 1.0 - f.y) * 0.10);   // distance to the nearest joint, metres
    float hb = h12(id), hc = h12(id + 31.7);
    vec3 tone = mix(vec3(0.60, 0.37, 0.27), vec3(0.74, 0.56, 0.40), hb);       // reddish-brown to tan
    tone = mix(tone, vec3(0.42, 0.27, 0.21), smoothstep(0.86, 0.97, hc));      // the odd dark brown block
    tone = mix(tone, vec3(0.80, 0.69, 0.55), smoothstep(0.88, 0.98, h12(id + 9.1)) * 0.8); // a sun-faded one
    tone *= 0.88 + 0.22 * vnoise(w * 42.0);                                  // pitted surface
    tone = mix(tone, tone * 1.22 + 0.04, smoothstep(0.016, 0.0, jd) * 0.8);  // worn edges go pale
    tone *= 0.93 + 0.14 * vnoise(q * 5.0 + id);                              // uneven wear inside a block
    float jointM = 1.0 - smoothstep(0.003, 0.010, jd);
    vec3 sand = mix(vec3(0.62, 0.53, 0.40), vec3(0.27, 0.22, 0.17), clamp(vnoise(w * 7.0) * 1.3 - 0.1, 0.0, 1.0)); // sand, dirt in places
    vec3 col = mix(tone, sand, jointM);
    col *= 0.84 + 0.20 * vnoise(w * 0.35) - 0.10 * smoothstep(0.62, 0.82, vnoise(w * 0.9 + 4.0)); // broad grime and dust
    // dry leaves: one per cell of a 1.1 m grid in about a quarter of the cells, a pointed ellipse in an autumn colour
    for (int k = 0; k < 2; ++k) {
        float sc = k == 0 ? 1.1 : 0.83;
        vec2 g = (w + float(k) * 3.7) / sc, gi = floor(g), gf = fract(g) - 0.5;
        float hh = h12(gi + float(k) * 11.0);
        if (hh > 0.74) {
            vec2 o = (vec2(h12(gi + 2.0), h12(gi + 5.0)) - 0.5) * 0.55;
            float an = h12(gi + 8.0) * 6.2831853;
            vec2 d = gf - o;
            vec2 r = vec2(cos(an) * d.x + sin(an) * d.y, -sin(an) * d.x + cos(an) * d.y) * sc;
            float leaf = 1.0 - smoothstep(0.55, 1.0, (r.x * r.x) / (0.058 * 0.058) + (r.y * r.y) / (0.024 * 0.024));
            vec3 lc = mix(vec3(0.42, 0.25, 0.09), vec3(0.62, 0.40, 0.14), h12(gi + 4.0));
            col = mix(col, lc * (0.9 + 0.2 * vnoise(w * 90.0)), leaf * 0.92);
        }
    }
    // litter: a pale scrap of paper or a cigarette end now and then
    vec2 lg = w / 1.7, li = floor(lg), lf = fract(lg) - 0.5;
    if (h12(li + 21.0) > 0.9) {
        vec2 d = lf - (vec2(h12(li + 3.0), h12(li + 6.0)) - 0.5) * 0.6;
        float scrap = step(abs(d.x), 0.018) * step(abs(d.y), 0.010);
        col = mix(col, vec3(0.80, 0.78, 0.72), scrap * 0.9);
    }
    return col;
}
float crackEdge(vec2 p) { // Worley edge distance: ~0 along the cell borders, which become the cracks
    vec2 i = floor(p), f = fract(p);
    float d1 = 8.0, d2 = 8.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = vec2(h12(i + g), h12(i + g + 17.0));
            float d = length(g + o - f);
            if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
        }
    return d2 - d1;
}
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
    if (uFoliage > 0.5) {
        // ---- foliage (ISSUES #314): per-tree colour variation, light shining through the leaves, a waxy sheen, breakup of the flat cards ----
        vec2 tc = floor(vWorld.xy * 0.16);
        float hh = fract(sin(dot(tc, vec2(12.9898, 78.233))) * 43758.5453);
        vec3 tintA = vec3(0.90, 1.05, 0.80), tintB = vec3(1.12, 1.03, 0.74), tintC = vec3(0.82, 0.98, 0.90);
        vec3 tint = hh < 0.5 ? mix(tintA, tintB, hh * 2.0) : mix(tintB, tintC, (hh - 0.5) * 2.0);
        vec3 leaf = c.rgb * tint;
        vec3 Vd = normalize(vWorld - uCam);
        float sunGlow = max(uDay, uSunset * 0.85); // the low sun still backlights the leaves (ISSUES #333)
        float back = pow(max(dot(Vd, uSun), 0.0), 3.0); // looking towards the sun: the leaves glow
        leaf += c.rgb * mix(vec3(0.60, 0.78, 0.14), vec3(1.0, 0.55, 0.15), uSunset) * back * 0.55 * sunGlow;
        leaf *= 0.80 + 0.30 * fract(sin(dot(floor(vWorld * 3.1), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        float sheen = pow(max(dot(reflect(Vd, vec3(0.0, 0.0, 1.0)), uSun), 0.0), 28.0) * 0.12 * sunGlow;
        c.rgb = leaf + vec3(sheen);
    }
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
        float isPave = step(250.5, fl) * step(fl, 251.5);                // pavement / plaza triangle (IsraelProps, ISSUES #300)
        float wall = (1.0 - smoothstep(0.25, 0.5, abs(Nt.z))) * t.a * (1.0 - isProp) * (1.0 - uFoliage);
        float flatUp = smoothstep(0.85, 0.97, Nt.z) * t.a * (1.0 - uFoliage);
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
            float roadM = flatUp * (1.0 - isProp) * (1.0 - smoothstep(0.22, 0.45, satO)) * (1.0 - smoothstep(0.42, 0.62, mxO)) * (1.0 - isBike) * (1.0 - isPave);
            if (roadM * nearM > 0.01) {
                vec2 ru = vWorld.xy / 3.4;
                vec3 col = matC(uMC0, ru) * vec3(0.80, 0.79, 0.78);
                col *= mix(1.0, clamp(mxO / 0.25, 0.55, 1.3), 0.45);       // keep the original's tyre marks and patches
                float bl = mix(1.0, bumpL(matN(uMN0, ru), Tg, Bg, up), 0.8);
                c.rgb = mix(c.rgb, col * vColor.rgb * 1.15 * bl, roadM * nearM * 0.92);
            }
            // ---- a worn, weathered road surface (ISSUES #299) -----------------------------------------------------------------------
            if (roadM > 0.01) {
                vec2 w2 = vWorld.xy;
                float nearC = 1.0 - smoothstep(30.0, 55.0, vDist);          // cracks and gravel only close up (no shimmer far away)
                float macro = vnoise(w2 * 0.045) * 0.6 + vnoise(w2 * 0.17) * 0.4;
                float repair = smoothstep(0.60, 0.625, vnoise(w2 * 0.23 + 7.0));  // patched stretches: darker, smoother, with hard edges
                float rough = clamp(0.62 + 0.3 * (macro - 0.5) * 2.0 - 0.34 * repair, 0.18, 0.95);
                vec3 road = c.rgb;
                road *= 0.88 + 0.26 * macro;                                // broad tonal drift of the surface
                road *= 1.0 - 0.20 * repair;
                vec2 warp = vec2(vnoise(w2 * 2.3), vnoise(w2 * 2.3 + 9.0)) - 0.5;
                float ce = crackEdge(w2 / 0.8 + warp * 0.9);                // wiggly, small-celled
                float crk = (1.0 - smoothstep(0.0, 0.028, ce)) * smoothstep(0.62, 0.76, vnoise(w2 * 0.13 + 3.0)) * (1.0 - repair);
                float hair = (1.0 - smoothstep(0.0, 0.012, crackEdge(w2 / 0.27 + warp * 1.4))) * smoothstep(0.55, 0.8, vnoise(w2 * 0.31 + 5.0));
                road *= 1.0 - (0.34 * crk + 0.16 * hair) * nearC;           // sparse, fine, dark cracks in the worn stretches
                float oil = smoothstep(0.74, 0.80, vnoise(w2 * 0.8 + 11.0)) * smoothstep(0.45, 0.65, vnoise(w2 * 0.09));
                road *= 1.0 - 0.28 * oil;                                   // oil and rubber stains
                road += step(0.992, h12(floor(w2 * 55.0))) * 0.10 * nearC;   // a few bright grains of aggregate
                // sheen: Fresnel reflection of the sky (the far road turns glossy at a grazing angle) and a sun glint
                vec3 Vr = normalize(uCam - vWorld);
                vec3 Lr = normalize(vec3(0.40, 0.30, 0.866));
                float fr = 0.035 + 0.965 * pow(1.0 - clamp(Vr.z, 0.0, 1.0), 5.0);
                vec3 skyC = mix(vec3(0.07, 0.10, 0.18), vec3(0.60, 0.72, 0.90), uDay) * mix(0.55, 1.0, clamp(1.0 - Vr.z, 0.0, 1.0));
                vec3 Hr = normalize(Lr + Vr);
                float spec = pow(max(Hr.z, 0.0), mix(24.0, 90.0, 1.0 - rough)) * (1.0 - rough) * 0.55 * uDay;
                road += skyC * fr * (1.0 - rough) * 0.6 + vec3(1.0, 0.95, 0.85) * spec;
                c.rgb = mix(c.rgb, road, roadM * (1.0 - smoothstep(250.0, 520.0, vDist)));
            }
            // worn lane paint: yellow centre lines chipped and faded unevenly
            {
                float yel = flatUp * (1.0 - isProp) * smoothstep(0.30, 0.50, satO) * smoothstep(0.35, 0.55, mxO);
                if (yel > 0.01) {
                    float wearP = 0.70 + 0.30 * vnoise(vWorld.xy * 5.5) * (0.6 + 0.4 * vnoise(vWorld.xy * 1.3));
                    c.rgb *= mix(1.0, wearP, yel);
                    c.rgb = mix(vec3(dot(c.rgb, vec3(0.3, 0.59, 0.11))), c.rgb, 1.0 + 0.18 * yel); // a touch more punch
                }
            }
            // pavements and plazas: worn Eckerstein paving (ISSUES #300)
            float paveM = isPave * flatUp * (1.0 - isProp) * (1.0 - roadM);
            if (paveM * nearM > 0.01) {
                float fadeP = smoothstep(30.0, 75.0, vDist);
                vec3 col = mix(paverTex(vWorld.xy), vec3(0.58, 0.42, 0.32), fadeP); // far away: the average tone, no moire
                c.rgb = mix(c.rgb, col * vColor.rgb * 1.2, paveM * nearM * 0.96);
                if (uDebugPave > 0.5) c.rgb = mix(c.rgb, vec3(1.0, 0.0, 1.0), paveM * 0.8);
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
        // painted kerbs: 1 m stripes, red-white (no parking) or blue-white (paid parking) by 48 m stretch of street. Weathered (ISSUES #300):
        // sun-faded paint that is chipped away to the bare concrete, mostly along the edges, with grime at the foot where the kerb meets
        // the asphalt, dust on the top edge and rain streaks. vColor.b carries the height up the face (0 foot, 1 top; set in IsraelProps).
        {
            float seg = floor(along / 1.0);
            float stripe = mod(seg, 2.0);
            float zone = h12(floor(vWorld.xy / 48.0) + 3.0);
            float blueZone = step(0.5, zone);
            float kv = clamp(vColor.b, 0.0, 1.0);
            vec3 paint = stripe < 0.5 ? vec3(0.86, 0.85, 0.80) : mix(vec3(0.66, 0.17, 0.15), vec3(0.15, 0.30, 0.58), blueZone);
            float st = h12(vec2(seg, floor(vWorld.x / 48.0) + floor(vWorld.y / 48.0) * 13.0)); // each stripe has aged differently
            paint = mix(paint, vec3(dot(paint, vec3(0.33))) * vec3(1.0, 0.98, 0.94), 0.22 + 0.32 * st); // sun-bleached
            paint *= 0.80 + 0.26 * st;
            vec3 concrete = vec3(0.60, 0.58, 0.54) * (0.85 + 0.30 * vnoise(vec2(along * 14.0, kv * 10.0)));
            float n1 = vnoise(vec2(along * 7.0, kv * 14.0 + seg * 3.1)), n2 = vnoise(vec2(along * 31.0, kv * 55.0));
            float edgeW = smoothstep(0.55, 0.95, kv) * 0.28 + (1.0 - smoothstep(0.0, 0.3, kv)) * 0.12;
            float chip = smoothstep(0.50, 0.68, n1 * 0.62 + n2 * 0.38 + edgeW + 0.16 * (zone - 0.5));
            vec3 kc = mix(paint, concrete, chip);
            float seam = 1.0 - smoothstep(0.0, 0.36, kv);
            kc = mix(kc, vec3(0.20, 0.17, 0.14), seam * (0.55 + 0.35 * vnoise(vec2(along * 6.0, 3.0))));
            kc *= 1.0 - 0.18 * vnoise(vec2(along * 45.0, 7.0)) * (1.0 - kv);                    // rain streaks run down the face
            kc = mix(kc, kc * 1.18 + 0.03, smoothstep(0.88, 1.0, kv) * 0.7);                       // the rubbed, dusty top edge
            c.rgb = mix(c.rgb, kc * (0.75 + 0.45 * vColor.r), isKerb * 0.97);
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
    float lift = max(uDay, 0.62 * uSunset); // the sunset street is lit, not merely dark (#333)
    {   // a tone curve, not a gain: shadows lift 2.3x, the highlights stay below white (no blown walls)
        vec3 x = c.rgb * vec3(1.65, 1.58, 1.45);
        c.rgb = mix(c.rgb, x / (1.0 + x) * 1.28, lift);
    }
    c.rgb = mix(c.rgb, vec3(dot(c.rgb, vec3(0.333))), 0.10 * lift);
    // a low sun warms everything: a faint gold through the afternoon, the full orange by the horizon (ISSUES #333)
    float warm = 1.0 - smoothstep(0.08, 0.85, uSun.z);
    c.rgb = mix(c.rgb, c.rgb * vec3(1.12, 0.93, 0.82) + vec3(0.030, 0.010, 0.0), warm * 0.55);
    // grade: a little saturation and a gentle shadow lift so the baked lighting does not crush to black
    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    c.rgb = mix(vec3(l), c.rgb, 1.0 + 0.16 * uGrade);
    c.rgb += (1.0 - l) * 0.018 * uGrade * vec3(0.55, 0.7, 1.0);
    if (uSpotOn > 0.01) {
        // the headlights: two cones thrown down the road, bright where they meet a surface that faces the lamp, fading with the square of the distance
        vec3 Ns = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
        vec3 add = vec3(0.0);
        for (int i = 0; i < 2; ++i) {
            vec3 p = i == 0 ? uSpotPos0 : uSpotPos1;
            vec3 d = vWorld - p;
            float dist = length(d);
            vec3 dir = d / max(dist, 0.01);
            float cd = dot(dir, uSpotDir);
            float cone = smoothstep(0.80, 0.965, cd);
            float core = pow(max((cd - 0.90) / 0.10, 0.0), 2.0);
            float atten = 1.0 / (1.0 + 0.0065 * dist * dist);
            float ndl = 0.45 + 0.55 * clamp(dot(Ns, -dir), 0.0, 1.0); // a road is lit at a glancing angle, but headlights are strong
            add += vec3(1.0, 0.92, 0.76) * (cone * 0.65 + core * 1.3) * atten * ndl;
        }
        // soft-clipped so a surface close to the lamp brightens but never burns out to white
        float lk = 1.0 - exp(-dot(add, vec3(0.333)) * 2.0 * uSpotOn);
        c.rgb = c.rgb * (1.0 + 0.85 * lk) + vec3(1.0, 0.92, 0.76) * 0.15 * lk;
    }
    // distance fog towards the horizon colour (smooth, then a slow exponential tail)
    float f = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
    f = f * f * (3.0 - 2.0 * f);
    c.rgb = mix(c.rgb, uFogColor, f);
    c.rgb *= uGlow;
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
uniform float uDay;
uniform vec3 uSun;      // towards the sun (time of day, ISSUES #333)
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
            // the sun mirrored in the glass (ISSUES #305): a hot spot on windows that face it, strongest at glancing angles
            vec3 Rg = reflect(d, N);
            glass += vec3(1.0, 0.93, 0.78) * pow(max(dot(Rg, uSun), 0.0), 160.0) * (1.2 + 3.0 * F) * smoothstep(0.25, 0.75, uDay);
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
uniform vec3 uLamp;   // x headlights, y brake pedal, z reverse (0..1)
uniform float uGlass; // 1 while the translucent lamp covers are drawn
uniform float uDay;   // 0 at night .. 1 in full daylight: the lamps burn less hot against the bright scene
uniform vec3 uSun;    // towards the sun (time of day, ISSUES #333)
uniform vec3 uSunCol; // the sun's colour: white at noon, orange at sunset, dim blue at night
uniform float uSunset; // 0 .. 1 while the sun is near the horizon
uniform float uStage; // 1 in the menu showroom (ISSUES #304): studio strip lights instead of the city
uniform vec3 uStageA, uStageB;
uniform float uWet;   // 0 dry .. 1 soaked (ISSUES #354): rain on the paint, drops, trails, a water film on the clear coat and the glass
uniform float uSpeed; // the car's speed in m/s: the wind drags the drops over the panels
// Per-car data for grounding and the lamp internals (ISSUES #312): the lowest point of the car, the lamp centres (left / right head, left /
// right tail; far away when the car has none) and the car's forward axis.
uniform float uBase;
uniform vec3 uHead0, uHead1, uTail0, uTail1, uFwdC;
varying float vAO;
varying vec4 vColor;
varying vec2 vUV;
varying vec3 vNormal;
varying vec3 vPos;
varying float vGloss;
varying float vDist;

float h13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float vn2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = h13(vec3(i, 1.0)), b = h13(vec3(i + vec2(1, 0), 1.0)), c = h13(vec3(i + vec2(0, 1), 1.0)), d = h13(vec3(i + vec2(1, 1), 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm2(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) {
        s += a * vn2(p);
        p = p * 2.03 + vec2(17.1, 9.2);
        a *= 0.5;
    }
    return s;
}

// Studio: strip lights for the menu showroom.
vec3 studioEnv(vec3 R) {
    float h = R.z;
    vec3 col = mix(vec3(0.018, 0.020, 0.028), vec3(0.075, 0.09, 0.13), smoothstep(-0.2, 0.95, h));
    col += vec3(1.0, 0.97, 0.92) * smoothstep(0.50, 0.88, h) * exp(-pow(R.y * 4.2, 2.0)) * 2.0;
    float a = atan(R.y, R.x);
    float band = smoothstep(-0.12, 0.22, h) * (1.0 - smoothstep(0.55, 0.92, h));
    col += uStageA * exp(-pow((a - 1.25) * 4.2, 2.0)) * band * 2.4;
    col += uStageB * exp(-pow((a + 1.25) * 4.2, 2.0)) * band * 2.4;
    col += mix(uStageA, vec3(1.0), 0.35) * pow(max(0.0, dot(R, normalize(vec3(-1.0, 0.15, 0.35)))), 24.0) * 1.5;
    col += vec3(1.0, 0.95, 0.9) * pow(max(0.0, dot(R, normalize(vec3(0.7, -0.1, 0.25)))), 40.0) * 0.8;
    return col;
}
// Daylight: what a clear coat really reflects in the street - the blue sky with drifting clouds and a hot sun, a skyline of buildings along the
// horizon, and the dark, warm road below.
vec3 dayEnv(vec3 R) {
    float h = R.z;
    vec3 haze = vec3(0.80, 0.85, 0.92), mid = vec3(0.30, 0.56, 0.92), zen = vec3(0.09, 0.26, 0.66);
    vec3 sky = mix(haze, mid, smoothstep(0.0, 0.28, h));
    sky = mix(sky, zen, smoothstep(0.18, 0.95, h));
    float cl = fbm2(R.xy / (h + 0.32) * 1.5 + vec2(uTime * 0.01, 0.0));
    sky = mix(sky, vec3(1.0, 1.0, 1.0), smoothstep(0.50, 0.78, cl) * smoothstep(0.02, 0.30, h) * 0.9);
    float sd = max(dot(R, uSun), 0.0);
    sky += uSunCol * (pow(sd, 48.0) * 0.55 + pow(sd, 700.0) * 14.0);
    sky = mix(sky, uSunCol * vec3(0.95, 0.72, 0.62), smoothstep(0.55, 0.0, abs(R.z)) * 0.18 * uSunset);
    float a = atan(R.y, R.x);
    float skyline = 0.07 + 0.11 * vn2(vec2(a * 4.0, 1.0)) + 0.07 * vn2(vec2(a * 13.0, 5.0)) + 0.04 * vn2(vec2(a * 41.0, 9.0));
    float bld = smoothstep(skyline, skyline - 0.025, h) * step(0.0, h);
    vec3 wall = mix(vec3(0.32, 0.31, 0.32), vec3(0.62, 0.58, 0.52), vn2(vec2(a * 9.0, h * 22.0)));
    wall *= 0.55 + 0.45 * smoothstep(0.0, skyline, h); // window bands get brighter towards the roofs
    vec3 road = mix(vec3(0.17, 0.165, 0.165), vec3(0.055, 0.055, 0.06), smoothstep(0.0, -0.7, h));
    return h < 0.0 ? road : mix(sky, wall, bld * 0.9);
}
// Night: a sodium horizon glow, rows of distant city lights and a couple of soft light boxes.
vec3 nightEnv(vec3 R) {
    float h = R.z;
    vec3 zenith = vec3(0.03, 0.05, 0.13), horizon = vec3(0.30, 0.27, 0.30), ground = vec3(0.045, 0.045, 0.055);
    vec3 col = mix(ground, mix(horizon, zenith, clamp(h * 1.6, 0.0, 1.0)), smoothstep(-0.12, 0.08, h));
    col += vec3(0.75, 0.45, 0.22) * exp(-abs(h - 0.02) * 12.0) * 0.55;
    float a = atan(R.y, R.x);
    float lights = pow(max(0.0, sin(a * 41.0 + 0.7)), 36.0) * pow(max(0.0, sin(a * 13.0)), 4.0);
    col += vec3(1.0, 0.85, 0.55) * lights * exp(-abs(h - 0.07) * 34.0) * 1.3;
    col += vec3(0.55, 0.72, 1.0) * pow(max(0.0, dot(R, normalize(vec3(0.35, 0.45, 0.82)))), 70.0) * 1.35;
    col += vec3(1.0, 0.8, 0.6) * pow(max(0.0, dot(R, normalize(vec3(-0.6, -0.2, 0.77)))), 60.0) * 1.2;
    return col;
}
vec3 environment(vec3 R) {
    if (uStage > 0.5)
        return studioEnv(R);
    vec3 e = mix(nightEnv(R), dayEnv(R), smoothstep(0.15, 0.85, uDay));
    // the low sun smears a warm band along the horizon and a broad glare into the clear coat (ISSUES #333)
    float sd = max(dot(R, uSun), 0.0);
    e += uSunCol * uSunset * (pow(sd, 40.0) * 0.9 + exp(-abs(R.z) * 7.0) * 0.18);
    return e;
}

// The nearer of the two lamps of a pair and the offset from its centre, split into forward-perpendicular components: x across the car, y up.
void lampFrame(vec3 pos, vec3 a, vec3 b, out vec2 q, out float near) {
    vec3 c = distance(pos, a) < distance(pos, b) ? a : b;
    vec3 d = pos - c;
    vec3 side = normalize(vec3(uFwdC.y, -uFwdC.x, 0.0) + vec3(1e-5));
    q = vec2(dot(d, side), d.z);
    near = length(d);
}
float fogOf(float dist) {
    float f = clamp((dist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
    return f * f * (3.0 - 2.0 * f);
}
// Water drops on a panel (ISSUES #354).  Each drop is a spherical cap sitting on the surface at a contact angle of about 65 deg: its
// surface normal tilts away from the panel's by up to that angle at the rim, which is what makes it a lens.  Three scales (about 6 mm,
// 15 mm and 35 mm across; mostly the small ones) share the panel.  Gravity pulls the water down the panel's slope and the wind of the
// car's speed drags it rearwards along the surface; a drop is stretched along that flow in proportion to how strongly it acts, and the
// biggest ones leave a thin trail of water behind them.  Returns the coverage 0..1 and, for the nearest drop, its cap normal (world),
// the distance from its centre in drop radii, the unit offset in the (flow, across) frame, a trail mask and the frame itself.
float waterDrops(vec3 N, vec3 P, float wet, bool glass, out vec3 capN, out float dn, out vec2 nxy, out float trail, out vec3 fOut, out vec3 tOut) {
    capN = N; dn = 1.0; nxy = vec2(0.0); trail = 0.0;
    vec3 gr = vec3(0.0, 0.0, -1.0);
    vec3 fg = gr - N * dot(gr, N);                       // down the slope, in the panel
    float slope = length(fg);
    vec3 fgn = fg / max(slope, 1e-4);
    vec3 fw = -uFwdC - N * dot(-uFwdC, N);                // the air flows back over the car
    vec3 fwn = fw / max(length(fw), 1e-4);
    float windK = smoothstep(3.0, 22.0, uSpeed) * min(1.0, 0.25 + length(fw) * 1.3);
    vec3 fl = fgn * slope * (1.0 - windK) + fwn * windK;
    float flen = length(fl);
    vec3 f = flen > 0.04 ? fl / flen : (length(fw) > 0.02 ? fwn : vec3(1.0, 0.0, 0.0));
    f = normalize(f - N * dot(f, N) + vec3(1e-5));
    vec3 t = normalize(cross(N, f));
    fOut = f; tOut = t;
    vec2 uv = vec2(dot(P, f), dot(P, t));
    float stretch = clamp(flen, 0.0, 1.0);
    // drops are blown off the surfaces that face into the wind
    float headOn = max(dot(N, uFwdC), 0.0) * smoothstep(8.0, 30.0, uSpeed);
    float cover = 0.0, best = 1e9;
    const float sinA = 0.90;
    for (int i = 0; i < 3; ++i) {
        float sc = i == 0 ? 83.0 : (i == 1 ? 36.0 : 14.0);             // cells per metre: 12, 28 and 70 mm cells (drops 4-8, 10-19 and 24-48 mm across)
        float dens = (i == 0 ? 0.42 : (i == 1 ? 0.20 : 0.07)) * wet * (1.0 - 0.7 * headOn * (i == 2 ? 1.0 : 0.5));
        vec2 g = uv * sc;
        vec2 cell = floor(g);
        vec2 loc = fract(g) - 0.5;
        float hr = h13(vec3(cell, 3.7 + float(i) * 11.0));
        float present = step(1.0 - dens, hr);
        vec2 off = (vec2(h13(vec3(cell, 8.1 + float(i))), h13(vec3(cell, 12.9 + float(i))) ) - 0.5) * 0.30;
        float sz = h13(vec3(cell, 21.3 + float(i)));
        float rad = 0.17 + 0.17 * sz;
        // the drop is stretched along the flow: a sagging teardrop on the upright panels, a streak where the wind has it
        float e = 1.0 + stretch * (0.35 + 1.5 * sz) * (i == 0 ? 0.45 : 1.0);
        e = min(e, 0.47 / rad);
        vec2 dv = loc - off;
        // a teardrop: rounder at the leading edge (down-stream), pinched at the tail
        float tail = smoothstep(0.0, -rad * e, dv.x);
        vec2 q = vec2(dv.x / (e * mix(1.0, 0.72, tail * stretch)), dv.y);
        float d = length(q) / rad;
        // a drop smaller than a pixel cannot be drawn without sparkling: it fades into the film instead
        float px = length(fwidth(P));
        float resolved = smoothstep(0.8 * px, 2.2 * px, 2.0 * rad / sc);
        float inside = present * resolved * (1.0 - smoothstep(0.90, 1.0, d));
        // the thin trail of water a big drop has left on its way down (only on the 12 mm scale, on tilted panels)
        if (i == 2 && present > 0.5) {
            float behind = off.x - e * rad * 0.8 - loc.x;
            float wtr = rad * 0.20 * (1.0 - clamp(behind / 0.5, 0.0, 1.0));
            float tr = step(0.0, behind) * (1.0 - smoothstep(wtr * 0.6, wtr, abs(dv.y))) * smoothstep(0.0, 0.05, 0.5 - loc.x - 0.0) * stretch;
            trail = max(trail, tr * (1.0 - inside));
        }
        if (inside > 0.0 && d < best) {
            best = d;
            cover = max(cover, inside);
            vec2 n2 = q / rad;
            float dd = length(n2);
            nxy = dd > 1e-3 ? n2 / dd : vec2(0.0);
            float sinT = sinA * min(dd, 1.0);
            capN = normalize(N * sqrt(max(1.0 - sinT * sinT, 0.03)) + (f * n2.x + t * n2.y) * sinA);
            dn = d;
        } else {
            cover = max(cover, inside);
        }
    }
    return cover;
}
void main() {
    vec4 tex = texture2D(uTex, vUV);
    vec4 base = tex * vColor;
    if (vGloss > 7.5 && vGloss < 9.5) {
        // ---- lamp covers (ISSUES #286): 8 clear headlight glass, 9 red brake-light glass. Drawn in their own blended pass. ----
        if (uGlass < 0.5)
            discard;
        vec3 Ng = normalize(vNormal);
        vec3 Vg = normalize(uCam - vPos);
        float NdVg = max(dot(Ng, Vg), 0.0);
        float fr = 0.06 + 0.94 * pow(1.0 - NdVg, 4.0);
        vec3 refl = environment(reflect(-Vg, Ng));
        vec3 tint = clamp(vColor.rgb, 0.0, 1.0);
        vec3 colG;
        float aG;
        if (vGloss < 8.5) {
            colG = refl * (0.40 + 0.80 * fr) + tint * 0.08 + vec3(1.0, 0.96, 0.82) * uLamp.x * 0.30;
            aG = clamp(0.14 + 0.60 * fr + 0.10 * uLamp.x, 0.0, 0.82);
        } else {
            colG = tint * (0.30 + 1.1 * uLamp.y) + refl * fr * 0.9;
            aG = clamp(0.48 + 0.30 * fr, 0.0, 0.9);
        }
        gl_FragColor = vec4(mix(colG, uFogColor, fogOf(vDist)), aG);
        return;
    }
    if (vGloss > 9.5 && vGloss < 10.5) {
        // the centre brake light: a red LED bar
        vec3 Nc = normalize(vNormal);
        vec3 Vc = normalize(uCam - vPos);
        float core = pow(max(dot(Nc, Vc), 0.0), 1.5);
        vec2 led = fract(vPos.xy * 90.0 + vPos.z * 40.0) - 0.5;
        float dots = smoothstep(0.42, 0.22, length(led));
        vec3 hotc = mix(vec3(0.85, 0.05, 0.03), vec3(1.0, 0.11, 0.06), uLamp.y) * (1.0 + 0.55 * uLamp.y) * (0.55 + 0.45 * core) * (0.55 + 0.6 * dots);
        vec3 colc = mix(clamp(vColor.rgb, 0.0, 1.0) * 0.30, hotc, clamp(0.30 + 0.70 * uLamp.y, 0.0, 1.0));
        gl_FragColor = vec4(mix(colc, uFogColor, fogOf(vDist)), 1.0);
        return;
    }
    if (vGloss > 5.5) {
        // ---- lamp lenses (ISSUES #284, #312): 6 headlight, 7 tail / brake light. Emissive with real internals: the headlight is a projector
        // (a hot bulb core, a lens ring, reflector facets and a thin LED daytime-running strip), the tail light a grid of red LEDs inside a
        // reflector ring that burns hotter with the brake pedal. -----------------------------------------------------------------------------
        vec3 Nl = normalize(vNormal);
        vec3 Vl = normalize(uCam - vPos);
        float NdVl = max(dot(Nl, Vl), 0.0);
        vec3 lens = clamp(vColor.rgb, 0.0, 1.0);
        vec3 col;
        vec2 q;
        float nearD;
        if (vGloss < 6.5) {
            lampFrame(vPos, uHead0, uHead1, q, nearD);
            float rr = length(q);
            float core = exp(-pow(rr / 0.045, 2.0));
            float ring = exp(-pow((rr - 0.105) / 0.016, 2.0));
            float facet = 0.75 + 0.25 * cos(atan(q.y, q.x) * 14.0);
            float bowl = smoothstep(0.17, 0.02, rr) * (0.45 + 0.55 * facet);
            float drl = exp(-pow((q.y - 0.085) / 0.011, 2.0)) * smoothstep(0.17, 0.10, abs(q.x));
            float on = uLamp.x;
            float bright = mix(2.1, 0.80, uDay);
            vec3 beam = vec3(1.0, 0.96, 0.82) * (core * 1.6 + bowl * 0.55 + ring * 0.9) * bright;
            vec3 strip = vec3(0.88, 0.95, 1.0) * drl * mix(1.2, 2.0, uDay);
            col = lens * (0.28 + 0.30 * NdVl) + (beam * on + strip * clamp(0.35 + 0.65 * on, 0.0, 1.0)) * (0.55 + 0.45 * NdVl);
        } else {
            lampFrame(vPos, uTail0, uTail1, q, nearD);
            vec2 g = fract(q / 0.026) - 0.5;
            float dots = smoothstep(0.40, 0.20, length(g));
            float rimGlow = exp(-pow((length(q * vec2(0.55, 1.2)) - 0.10) / 0.03, 2.0));
            float brake = uLamp.y;
            float on = clamp(0.30 + 0.70 * brake, 0.0, 1.0);
            vec3 red = mix(vec3(0.80, 0.04, 0.02), vec3(1.0, 0.10, 0.05), brake);
            vec3 hot = red * (0.45 + 0.85 * dots + 0.5 * rimGlow) * (0.9 + 0.8 * brake) * (0.7 + 0.5 * pow(NdVl, 1.5));
            hot += vec3(0.0, 0.10, 0.04) * brake * dots * pow(NdVl, 3.0);
            // the reverse lamp: a white LED block
            hot += vec3(1.0, 0.95, 0.85) * uLamp.z * exp(-pow((q.x - 0.10) / 0.06, 2.0)) * 1.4;
            col = mix(lens * 0.30, hot, on);
        }
        col += vec3(pow(1.0 - NdVl, 4.0) * 0.12) + environment(reflect(-Vl, Nl)) * 0.12; // glassy rim light
        gl_FragColor = vec4(mix(col, uFogColor, fogOf(vDist)), 1.0);
        return;
    }
    if (vGloss < 0.5) {
        // tyres, lights, badges: the CPU-lit vertex colour
        gl_FragColor = vec4(mix(base.rgb, uFogColor, fogOf(vDist)), base.a);
        return;
    }
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCam - vPos);
    vec3 L = uSun;
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
            vec3 paint = clamp(vColor.rgb, 0.0, 1.0);
            vec3 F0 = mix(vec3(0.62), paint * 0.8 + 0.2, 0.5);
            vec3 F = F0 + (1.0 - F0) * pow(1.0 - NdV, 5.0);
            float diffuse = 0.22 + 0.40 * NdL;
            col = paint * diffuse * 0.9 + env * F * 2.1 + F0 * (pow(NdH, 90.0) * 1.6 + pow(NdH, 14.0) * 0.25) * (0.35 + 0.65 * NdL);
            col *= vAO;
            vec3 disc = (paint * 0.35 + env * F0 * 0.35) * (0.55 + 0.45 * vAO);
            col = mix(col, disc, uBlur * 0.8);
        } else if (vGloss < 4.5) {
            vec3 print = tex.rgb * vColor.rgb;
            float luma = dot(print, vec3(0.299, 0.587, 0.114));
            vec3 rubber = vec3(0.022, 0.022, 0.025) + vec3(luma) * 0.42;
            float sheen = pow(1.0 - NdV, 3.0) * 0.14 + pow(NdH, 24.0) * 0.07;
            col = rubber * (0.55 + 0.55 * NdL) + env * (0.03 + sheen) + vec3(sheen * 0.4);
            col *= mix(1.0, vAO, 0.6);
        } else {
            vec3 steel = vec3(0.34, 0.34, 0.36);
            float groove = 0.9 + 0.1 * sin(length(vPos.xz) * 520.0);
            vec3 F = vec3(0.5) + 0.5 * pow(1.0 - NdV, 5.0);
            col = steel * (0.25 + 0.4 * NdL) * groove + env * F * 0.55 + vec3(pow(NdH, 40.0) * 0.5);
            col *= 0.55 + 0.45 * vAO;
        }
        // wheels sit in the arches: darker towards the ground contact and the inner barrel
        float lowW = 1.0 - smoothstep(0.0, 0.40, vPos.z - uBase);
        col *= 1.0 - 0.28 * lowW;
        gl_FragColor = vec4(mix(col, uFogColor, fogOf(vDist)), (vGloss < 3.5) ? 1.0 : tex.a);
        return;
    }
    // ---- paint (gloss 1) and glass (gloss 2): clear coat with orange peel and metallic flakes, an environment that is the real
    // street, and contact with the ground (ISSUES #312) --------------------------------------------------------------------------------
    bool glass = vGloss > 1.5;
    float near = 1.0 - smoothstep(10.0, 42.0, vDist);
    vec3 Np = N;
    if (!glass && near > 0.0) { // orange peel: a faint, fine ripple in the surface normal that breaks up reflections (a water film fills it in)
        vec3 pn = vec3(vn2(vPos.xy * 70.0 + vPos.z * 41.0), vn2(vPos.yz * 70.0 + 11.0), vn2(vPos.zx * 70.0 + 23.0)) - 0.5;
        Np = normalize(N + pn * 0.055 * near * (1.0 - 0.85 * uWet));
    }
    // rain (ISSUES #354): water drops sit on the panel as small lenses (see waterDrops); the rest of the surface carries a thin film
    float wetK = uWet * near;
    vec3 capN = N, fDir = vec3(1.0, 0.0, 0.0), tDir = vec3(0.0, 1.0, 0.0);
    float dropCover = 0.0, dropD = 1.0, trail = 0.0;
    vec2 dropNxy = vec2(0.0);
    if (wetK > 0.01)
        dropCover = waterDrops(N, vPos, wetK, glass, capN, dropD, dropNxy, trail, fDir, tDir);
    float film = clamp(0.55 * wetK + 0.35 * trail * wetK, 0.0, 1.0);   // how much water covers this spot
    float NdV = max(dot(Np, V), 0.0);
    float NdL = dot(Np, L);
    float wrap = clamp((NdL + 0.30) / 1.30, 0.0, 1.0);
    bool bright = uDay > 0.5 || uSunset > 0.35 || uStage > 0.5; // daylight, a low sun, or the lit showroom
    float dk = smoothstep(0.0, 0.75, uDay);   // continuous day/night ambient (ISSUES #333)
    vec3 skyAmb = mix(vec3(0.05, 0.07, 0.13), vec3(0.36, 0.50, 0.78), dk);
    vec3 grdAmb = mix(vec3(0.05, 0.045, 0.045), vec3(0.30, 0.285, 0.27), dk);
    vec3 amb = mix(grdAmb, skyAmb, 0.5 + 0.5 * Np.z) * mix(1.0, 0.62, dk);
    // the direct sun still lights the paint at sunset: it is orange and low, not gone (ISSUES #333)
    vec3 sunCol = mix(vec3(0.14, 0.16, 0.24), uSunCol, smoothstep(-0.04, 0.10, uSun.z));
    float ao = mix(1.0, vAO, 0.6);
    vec3 lit = base.rgb * (amb * ao + sunCol * wrap * (uStage > 0.5 ? 0.6 : 1.0) * 0.95);
    lit *= 1.0 - 0.34 * film * (glass ? 0.0 : 1.0);   // wet paint reads darker and richer: the water film cuts the diffuse scatter
    lit = mix(lit, lit * lit * 1.8, 0.18 * film * (glass ? 0.0 : 1.0));  // and a little more saturated
    // sitting on the road: the lower panels fall into the shadow of the car itself and pick up the dark asphalt
    float rel = vPos.z - uBase;
    float low = 1.0 - smoothstep(0.04, 0.60, rel);
    lit *= 1.0 - 0.52 * low;
    lit += vec3(0.020, 0.019, 0.018) * low * (0.5 - 0.5 * Np.z);
    // clear coat: Fresnel-weighted environment reflection + a hot, tight sun lobe + a broad sheen
    vec3 R = reflect(-V, Np);
    float F0 = (glass ? 0.10 : 0.068) + 0.055 * film;                               // water on clear coat: a second, brighter interface
    float fres = F0 + (1.0 - F0) * pow(1.0 - NdV, 5.0);
    float reflectivity = glass ? 1.0 : mix(0.95, 1.0, film);
    vec3 env = environment(R);
    vec3 H = normalize(L + V);
    float NdH = max(dot(Np, H), 0.0);
    float tight = pow(NdH, glass ? 420.0 : mix(260.0, 520.0, film)) * (2.4 + 2.2 * film);
    float broad = pow(NdH, 28.0) * 0.20 * (1.0 - 0.5 * film);
    vec3 spec = sunCol * (tight + broad) * smoothstep(-0.05, 0.25, NdL) * (bright ? 1.0 : 0.5);
    // metallic flakes: tiny mirrors scattered in the paint that glint with the view
    float flake = 0.0;
    if (!glass && near > 0.0) {
        vec3 cell = floor(vPos * 230.0);
        float rr = h13(cell);
        vec3 fn = normalize((vec3(h13(cell + 1.3), h13(cell + 5.7), h13(cell + 9.1)) * 2.0 - 1.0) * 0.8 + N * 1.4);
        float sp = pow(max(dot(reflect(-V, fn), L), 0.0), 40.0) + 0.35 * pow(max(dot(reflect(-V, fn), normalize(R + vec3(0.0, 0.0, 0.5))), 0.0), 18.0);
        flake = step(0.93, rr) * sp * 0.55 * near * (bright ? 1.0 : 0.35) * (1.0 - 0.6 * film);   // flakes sit under the water: they sparkle less
    }
    vec3 col = lit * (1.0 - fres * reflectivity * 0.62) + env * fres * reflectivity * (1.0 - 0.35 * low) * (1.0 + 0.55 * film) + spec + (0.35 + base.rgb * 1.6) * flake;
    col += env * film * (glass ? 0.0 : 0.10) * (0.4 + 0.6 * fres);  // the water sheet: a broad, glassy second reflection
    if (glass) {
        // tinted glass: dark interior seen through it, almost everything else is reflection
        col = base.rgb * amb * 0.30 + env * (0.38 + 0.95 * fres) * vec3(0.92, 1.0, 0.98) + spec * 1.1;
    }
    // water drops, each a lens (ISSUES #354): the Fresnel reflection of the surroundings in the cap (about 2 % head on, almost a mirror at the
    // rim), the paint (or the dark cabin) seen through the water a little darker and richer, the sun's catch-light on the cap, a bright
    // crescent inside the drop on the side away from the light where the refracted light lands, and the rim where the view turns grazing
    if (dropCover > 0.0) {
        vec3 Rd = environment(reflect(-V, capN));
        float Fw = 0.02 + 0.98 * pow(1.0 - max(dot(capN, V), 0.0), 5.0);
        float cap = pow(max(dot(capN, H), 0.0), 150.0) * 3.0 * smoothstep(-0.05, 0.25, dot(N, L));
        float rim = smoothstep(0.70, 0.97, dropD);
        vec2 ld = normalize(vec2(dot(L, fDir), dot(L, tDir)) + vec2(1e-4));
        float cres = smoothstep(0.25, 0.90, dot(dropNxy, -ld)) * smoothstep(0.45, 0.95, dropD) * smoothstep(-0.2, 0.3, dot(N, L));
        vec3 dropCol;
        if (glass) {
            vec3 refr = environment(normalize(vec3(-V.xy * 0.6, abs(V.z) + 0.3)));   // the world seen inverted through the little lens
            dropCol = mix(refr * 0.55 + col * 0.40, Rd, clamp(Fw * 0.9 + 0.15, 0.0, 1.0));
        } else {
            // the dome magnifies and brightens the paint beneath it, and mirrors the sky over it (added, not mixed: a dark ground
            // reflection must not turn the drop into a black speck)
            vec3 under = lit * 0.95 + base.rgb * 0.16;
            dropCol = under * 1.16 + Rd * (0.10 + 0.55 * Fw);
            dropCol += under * cres * 0.70;
        }
        dropCol += sunCol * cap;
        if (glass)
            dropCol = mix(dropCol, Rd * 0.9 + 0.12 * col, rim * 0.30);
        col = mix(col, dropCol, dropCover);
        // the thin dark line where the edge of the water meets the paint
        col *= 1.0 - 0.20 * smoothstep(0.86, 0.96, dropD) * (1.0 - smoothstep(0.96, 1.0, dropD)) * dropCover;
    }
    col = mix(col, col * 0.88 + env * 0.10, trail * wetK);   // the thin trail a drop leaves: a sliver of water
    gl_FragColor = vec4(mix(col, uFogColor, fogOf(vDist)), base.a);
}
)";

// ---- Post-processing: bright pass (4x down), separable blur, composite with tone curve, bloom and vignette -------------
// Procedural sky (ISSUES #302): a per-pixel view ray through a scattering-style gradient (deep zenith, pale hazy horizon that meets the
// fog), a sun with a disc, bloom-like halo and warm forward haze, two cloud layers (lit cumulus with self-shadowing, silver edges and a
// fading distance; high stretched cirrus) drifting with the wind, and at night a dusk gradient with stars and a moon.
inline const char *kSkyVS = R"(#version 120
varying vec2 vNdc;
void main() {
    gl_Position = gl_Vertex;
    vNdc = gl_Vertex.xy;
}
)";

inline const char *kSkyFS = R"(#version 120
varying vec2 vNdc;
uniform vec3 uF, uR, uU;
uniform vec2 uTan;
uniform vec3 uSun;      // towards the sun (time of day, ISSUES #333)
uniform vec3 uMoon;     // towards the moon (fixed: the night sky needs one)
uniform vec3 uHorizon;  // the time of day's haze / fog colour
uniform vec3 uSunCol;   // the direct light / disc colour: white at noon, orange at sunset
uniform float uSunset;  // 0 .. 1 while the sun is near the horizon
uniform float uTime;
uniform float uDay;     // 0 night .. 1 full daylight (continuous)
float h21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vn(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(h21(i), h21(i + vec2(1, 0)), f.x), mix(h21(i + vec2(0, 1)), h21(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 5; ++i) {
        s += a * vn(p);
        p = p * 2.03 + vec2(17.1, 9.2);
        a *= 0.5;
    }
    return s;
}
void main() {
    vec3 d = normalize(uF + vNdc.x * uTan.x * uR + vNdc.y * uTan.y * uU);
    float el = d.z;
    float sd = max(dot(d, uSun), 0.0);
    vec3 col;
    float day = clamp(uDay, 0.0, 1.0);
    // --- atmosphere ---
    vec3 zenith = mix(vec3(0.05, 0.10, 0.26), vec3(0.09, 0.27, 0.66), day);
    vec3 mid = mix(vec3(0.12, 0.20, 0.38), vec3(0.28, 0.56, 0.92), day);
    float e = clamp(el, 0.0, 1.0);
    vec3 haze = uHorizon * vec3(0.96, 0.97, 0.99);
    col = mix(haze, mid, smoothstep(0.0, 0.17, e));
    col = mix(col, zenith, smoothstep(0.10, 0.80, e));
    col = mix(col, haze, (1.0 - smoothstep(-0.05, 0.0, el)));     // below the horizon: the haze
    // the golden band of a sunrise / sunset, strongest on the sun's side of the horizon (ISSUES #333)
    float az = dot(normalize(d.xy + vec2(1e-5)), normalize(uSun.xy + vec2(1e-5))) * 0.5 + 0.5;
    col = mix(col, vec3(0.98, 0.46, 0.20) * (0.55 + 0.45 * pow(az, 6.0)), uSunset * (0.18 + 0.55 * exp(-e * 6.0)));
    // low sun scatter: a warm band around the sun's azimuth near the horizon, and a Mie halo around the sun itself
    float hz = exp(-e * 7.0);
    col += uSunCol * pow(sd, 5.0) * (0.10 + 0.30 * hz) * (0.25 + 0.75 * day + 0.9 * uSunset);
    col += uSunCol * pow(sd, 48.0) * 0.55 * max(day, uSunset * 0.85);
    col += vec3(1.0, 0.97, 0.90) * pow(sd, 700.0) * 2.4 * day * (1.0 - 0.35 * uSunset); // the glare
    float disc = smoothstep(0.99955, 0.99975, sd) * smoothstep(-0.02, 0.05, el);
    col = mix(col, uSunCol * 2.2, disc);
    // --- night: stars and a moon, only once the sun is below the horizon (ISSUES #333) ---
    float na = 1.0 - smoothstep(-0.14, 0.03, uSun.z); // 0 while the disc is up, 1 well after it sets
    if (na > 0.01) {
        vec2 sp = d.xy / max(d.z + 0.35, 0.2) * 70.0;
        float st = h21(floor(sp));
        float tw = 0.6 + 0.4 * sin(uTime * (1.5 + st * 4.0) + st * 50.0);
        col += vec3(0.9, 0.95, 1.0) * step(0.985, st) * tw * smoothstep(0.05, 0.4, el) * 0.8 * na;
        float md = max(dot(d, uMoon), 0.0);
        col = mix(col, vec3(0.88, 0.92, 1.0), smoothstep(0.9993, 0.9996, md) * 0.9 * na);
        col += vec3(0.30, 0.38, 0.55) * pow(md, 90.0) * 0.5 * na;
    }
    // --- clouds ---
    if (el > 0.0) {
        float t = uTime;
        // cumulus on a plane ~1.8 km up: perspective projection so the clouds shrink towards the horizon
        vec2 p = d.xy / (el + 0.05) * 0.80 + vec2(t * 0.012, t * 0.006);
        float cover = 0.50;
        float n = fbm(p * 1.3);
        float dens = smoothstep(cover, cover + 0.13, n + 0.14 * fbm(p * 4.3 + 5.0));
        // self-shadowing: sample towards the sun; thicker cloud on that side = darker base
        vec2 toSun = normalize(uSun.xy + 1e-4) * 0.07;
        float n2 = fbm((p + toSun) * 1.3);
        float shade = clamp(0.58 + (n - n2) * 8.0 - 0.25 * smoothstep(0.62, 0.8, n), 0.0, 1.1);
        vec3 lit = mix(vec3(0.52, 0.50, 0.55), vec3(1.02, 0.99, 0.94), day);
        lit = mix(lit, uSunCol * 1.05, uSunset * 0.8);
        vec3 shadow = mix(vec3(0.16, 0.18, 0.26), vec3(0.46, 0.54, 0.70), day);
        shadow = mix(shadow, uSunCol * 0.35, uSunset * 0.5);
        vec3 cc = mix(shadow, lit, clamp(shade, 0.0, 1.0)) * mix(0.22, 1.0, day);
        // bright rims where the cloud is thin and faces the sun (silver lining)
        float rim = (1.0 - dens) * smoothstep(0.0, 0.5, n - cover + 0.2) * pow(sd, 6.0);
        cc += mix(vec3(1.0, 0.95, 0.85), uSunCol, uSunset * 0.8) * rim * 1.2 * day;
        float fade = smoothstep(0.0, 0.22, el);                        // dissolve into the haze at the horizon
        float a = dens * fade * mix(0.35, 1.0, day);
        col = mix(col, cc, a);
        // cirrus: a thin streaked veil, high up
        vec2 q = d.xy / (el + 0.30) * vec2(0.9, 2.6) + vec2(t * 0.02, 0.0);
        float ci = smoothstep(0.52, 0.85, fbm(q * 2.2 + 11.0)) * smoothstep(0.02, 0.35, el) * 0.32;
        col = mix(col, mix(vec3(0.95, 0.97, 1.0), vec3(0.3, 0.35, 0.5), 1.0 - day), ci * mix(0.4, 1.0, day));
    }
    // film-style dither against banding in the smooth gradient
    col += (h21(gl_FragCoord.xy + fract(uTime) * 61.0) - 0.5) / 255.0;
    gl_FragColor = vec4(col, 1.0);
    if (uDay > 1.5) gl_FragColor = vec4(d * 0.5 + 0.5, 1.0);
}
)";

// The showroom stage backdrop (ISSUES #304): a dark studio with a radial accent glow behind the car, two soft light slabs and a faint
// horizon line.  Drawn as a full-screen quad before the car; the floor and the reflection are geometry.
inline const char *kStageBgFS = R"(#version 120
varying vec2 vNdc;
uniform vec3 uAccent, uAccent2;
uniform vec2 uCenter;      // where the car sits on screen (ndc)
uniform float uTime;
uniform float uHorizon;    // ndc y of the floor horizon
uniform float uAspect;
float h21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
void main() {
    vec2 p = vNdc;
    vec2 q = vec2((p.x - uCenter.x) * uAspect, p.y - uCenter.y);
    float up = smoothstep(-1.0, 0.8, p.y);
    vec3 col = mix(vec3(0.012, 0.016, 0.026), vec3(0.050, 0.064, 0.095), up);
    // a big soft glow behind the car, and a tighter hot core
    float r = length(q * vec2(0.62, 0.95));
    col += uAccent * 0.55 * exp(-r * r * 2.6) * (0.85 + 0.15 * sin(uTime * 0.7));
    col += uAccent * 0.35 * exp(-r * r * 14.0);
    // two vertical light slabs (softbox panels) either side, brightest mid-height
    for (int i = 0; i < 2; ++i) {
        float sx = uCenter.x + (i == 0 ? -1.0 : 1.0) * 0.95 * (1.0 + 0.03 * sin(uTime * 0.4 + float(i)));
        float dx = (p.x - sx) * uAspect;
        float slab = exp(-dx * dx * 55.0) * smoothstep(-0.2, 0.25, p.y) * (1.0 - smoothstep(0.55, 1.05, p.y));
        col += (i == 0 ? uAccent2 : uAccent) * 0.32 * slab;
    }
    // the floor: darker below the horizon, with a thin bright seam where it meets the wall
    float below = 1.0 - smoothstep(uHorizon - 0.02, uHorizon + 0.02, p.y);
    col = mix(col, col * 0.35 + vec3(0.006, 0.008, 0.012), below);
    col += mix(uAccent, vec3(1.0), 0.4) * 0.35 * exp(-pow((p.y - uHorizon) * 38.0, 2.0)) * exp(-p.x * p.x * 1.2);
    // vignette and grain
    col *= 1.0 - 0.55 * pow(length(p * vec2(0.8, 0.9)), 2.2);
    col += (h21(gl_FragCoord.xy + fract(uTime) * 71.0) - 0.5) / 120.0;
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

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
uniform float uMotion;   // speed effect strength 0..1 (ISSUES #298)
uniform float uNitro;    // 0..1, nitrous burst: a little more of everything
uniform vec2 uFxCenter;  // the vanishing point, uv
uniform vec2 uCarPos;    // the car's ellipse centre in uv
uniform vec2 uCarRad;    // and radii; x = 0: no car in view
uniform sampler2D uDepth; // the frame's depth: 1.0 = nothing drawn = sky (ISSUES #305)
uniform vec2 uSunUV;      // the sun on screen (uv, may lie outside)
uniform float uRays;      // god rays and lens reflections strength 0..1
uniform float uAspectX;   // width / height
uniform float uTime;
varying vec2 vUV;
float skyAt(vec2 uv) {
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y > 1.0 || uv.y < 0.0)
        return uv.y > 1.0 ? 1.0 : 0.0; // above the frame is open sky
    return step(0.99995, texture2D(uDepth, uv).r);
}
vec3 filmic(vec3 x) { // ACES fit
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
void main() {
    vec3 c;
    vec2 d = (vUV - uFxCenter);
    float amt = 0.0;
    if (uMotion > 0.002) {
        // Radial zoom blur streaking outwards from the vanishing point: none at the centre, growing towards the edges and the bottom;
        // the car stays sharp (NFS Unbound's speed look).
        float r = length(d * vec2(1.35, d.y < 0.0 ? 1.45 : 1.0)); // the road below the horizon streaks more
        float edge = smoothstep(0.06, 0.52, r);
        float carK = 1.0;
        if (uCarRad.x > 0.001)
            carK = smoothstep(0.72, 1.35, length((vUV - uCarPos) / uCarRad));
        amt = uMotion * edge * carK;
    }
    if (amt > 0.003) {
        float k = (0.17 + 0.07 * uNitro) * amt;
        float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        vec3 acc = vec3(0.0);
        float wsum = 0.0;
        for (int i = 0; i < 16; ++i) {
            float t = (float(i) + jitter) / 16.0;
            float wgt = 1.0 - 0.55 * t;
            acc += texture2D(uScene, vUV - d * (k * t)).rgb * wgt;
            wsum += wgt;
        }
        c = acc / wsum;
        // colour fringing towards the edges: the red and blue channels sampled a little apart along the radius
        float ca = 0.0055 * amt;
        c.r = mix(c.r, texture2D(uScene, vUV - d * ca).r, 0.5);
        c.b = mix(c.b, texture2D(uScene, vUV + d * ca).b, 0.5);
    } else {
        c = texture2D(uScene, vUV).rgb;
    }
    if (uRays > 1.5) { gl_FragColor = vec4(vec3(skyAt(vUV)), 1.0); return; } // NFSU2_RAYS=2: show the sky mask (dev aid)
    if (uRays > 0.002) {
        // ---- god rays: march from the pixel towards the sun, collecting open sky; buildings and trees cut the shafts ----
        vec2 toSun = uSunUV - vUV;
        vec2 aspect = vec2(uAspectX, 1.0);
        float dist = length(toSun * aspect);
        const int N = 48;
        vec2 stepv = toSun * 0.95 / float(N);
        float jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        vec2 p = vUV + stepv * jit;
        float illum = 1.0, acc = 0.0;
        for (int i = 0; i < N; ++i) {
            float sky = skyAt(p);
            vec3 sc = texture2D(uScene, clamp(p, 0.001, 0.999)).rgb;
            float lum = dot(sc, vec3(0.299, 0.587, 0.114));
            acc += sky * illum * (0.45 + 0.55 * smoothstep(0.25, 0.85, lum));
            illum *= 0.962;
            p += stepv;
        }
        float shaft = acc / float(N);
        shaft *= pow(1.0 - clamp(dist * 0.62, 0.0, 1.0), 1.4);              // thins out with distance from the sun
        float pixelSky = skyAt(vUV);
        shaft *= mix(1.0, 0.55, pixelSky);                                    // against open sky the shafts add less
        vec3 rayCol = vec3(1.0, 0.86, 0.62);
        c += rayCol * shaft * uRays * 0.95;
        // ---- lens reflections: ghosts along the line through the screen centre, a halo and a streak, only while the sun disc is visible ----
        float vis = 0.0;
        for (int k = 0; k < 5; ++k) {
            vec2 o = vec2(k == 0 ? 0.0 : (k == 1 ? 0.012 : (k == 2 ? -0.012 : 0.0)), k == 3 ? 0.012 : (k == 4 ? -0.012 : 0.0));
            vis += skyAt(uSunUV + o) * 0.2;
        }
        vis *= uRays * smoothstep(1.35, 0.7, max(abs(uSunUV.x - 0.5) * 2.0, abs(uSunUV.y - 0.5) * 2.0));
        if (vis > 0.002) {
            vec2 ctr = vec2(0.5);
            vec2 axis = uSunUV - ctr;
            vec2 q = (vUV - ctr) * aspect;
            vec3 fl = vec3(0.0);
            for (int i = 0; i < 5; ++i) {
                float kk = i == 0 ? -0.55 : (i == 1 ? 0.38 : (i == 2 ? 0.8 : (i == 3 ? -1.15 : 1.55)));
                float rr = i == 0 ? 0.055 : (i == 1 ? 0.035 : (i == 2 ? 0.095 : (i == 3 ? 0.045 : 0.07)));
                vec3 tint = i == 0 ? vec3(1.0, 0.65, 0.35) : (i == 1 ? vec3(0.35, 0.75, 1.0) : (i == 2 ? vec3(0.6, 1.0, 0.55) : (i == 3 ? vec3(1.0, 0.45, 0.7) : vec3(0.55, 0.6, 1.0))));
                vec2 gp = axis * kk * aspect;
                float d = length(q - gp) / rr;
                float disc = (1.0 - smoothstep(0.82, 1.0, d)) * 0.10 + exp(-pow((d - 0.92) * 9.0, 2.0)) * 0.15; // a filled disc with a bright rim
                fl += tint * disc;
            }
            // the halo ring around the sun and the horizontal anamorphic streak
            float ds = length((vUV - uSunUV) * aspect);
            fl += vec3(1.0, 0.85, 0.6) * exp(-pow((ds - 0.20) * 16.0, 2.0)) * 0.22;
            fl += vec3(1.0, 0.9, 0.75) * exp(-ds * 7.0) * 0.32;
            fl += vec3(0.55, 0.75, 1.0) * exp(-abs(vUV.y - uSunUV.y) * 70.0) * exp(-abs(vUV.x - uSunUV.x) * 1.6) * 0.45;
            c += fl * vis;
        }
    }
    vec3 b = texture2D(uBloom, vUV).rgb;
    c += b * uBloomStrength;
    c *= uExposure;
    c = mix(c, filmic(c), 0.55);                       // a soft filmic shoulder on top of the display curve
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, 1.08);                         // slight saturation
    vec2 dv = vUV - 0.5;
    c *= 1.0 - (uVignette + 0.16 * uMotion + 0.06 * uNitro) * smoothstep(0.18, 0.62, length(dv * vec2(1.0, 0.85)));
    gl_FragColor = vec4(c, 1.0);
}
)";

// ---- Flames (ISSUES #317): additive sprites for the nitrous exhaust jet, embers and glare ------------------------------------------------
inline const char *kFlameVS = R"(#version 120
varying vec2 vUV;
varying vec4 vColor;
void main() {
    gl_Position = ftransform();
    vUV = gl_MultiTexCoord0.xy;
    vColor = gl_Color;
}
)";

inline const char *kFlameFS = R"(#version 120
uniform sampler2D uTex;
uniform float uMode;  // 0 turbulent flame blob, 1 textured (fire atlas / glare), 2 spark streak, 3 soft round glow
uniform float uTime;
uniform float uSeed;
uniform vec2 uCell;   // atlas cell offset for mode 1 (0 or 0.5)
uniform float uCellScale;
varying vec2 vUV;
varying vec4 vColor;
float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(h21(i), h21(i + vec2(1.0, 0.0)), f.x), mix(h21(i + vec2(0.0, 1.0)), h21(i + vec2(1.0, 1.0)), f.x), f.y);
}
float fbm(vec2 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 4; ++i) {
        s += a * vnoise(p);
        p = p * 2.03 + vec2(17.1, 9.3);
        a *= 0.5;
    }
    return s;
}
void main() {
    vec2 p = vUV * 2.0 - 1.0;
    float r = length(p);
    vec3 col;
    if (uMode < 0.5) {
        // a lick of flame: noise pulls the edge about, so the blob tears into tongues that stream away from the exhaust (-y of the quad)
        vec2 q = p * vec2(2.4, 1.7) + vec2(uSeed * 11.0, uSeed * 5.0 - uTime * 7.5);
        float n = fbm(q);
        float n2 = fbm(q * 1.9 + vec2(4.0, -uTime * 11.0));
        float d = r + (n - 0.5) * 0.85 + (n2 - 0.5) * 0.35;
        float body = smoothstep(1.0, 0.05, d);
        float core = smoothstep(0.52, 0.0, d + 0.1 * r);
        col = vColor.rgb * body * (0.6 + 1.0 * n) + mix(vColor.rgb, vec3(1.0, 0.95, 0.88), 0.5) * core * core * 0.9;
        col *= vColor.a;
    } else if (uMode < 1.5) {
        vec4 t = texture2D(uTex, uCell + vUV * uCellScale);
        col = t.rgb * vColor.rgb * vColor.a * (0.5 + 0.9 * t.a);
    } else if (uMode < 2.5) {
        // spark: a thin bright streak, hot white at the head
        float along = vUV.y;
        float w = abs(p.x);
        float a = smoothstep(1.0, 0.0, w) * smoothstep(0.0, 0.25, along) * (0.25 + 0.75 * along);
        col = mix(vColor.rgb, vec3(1.0, 0.9, 0.7), smoothstep(0.7, 1.0, along)) * a * vColor.a;
    } else {
        float a = exp(-r * r * 3.2) * (1.0 - smoothstep(0.85, 1.0, r));
        col = vColor.rgb * a * vColor.a;
    }
    gl_FragColor = vec4(col, 1.0);
}
)";

} // namespace Shaders
