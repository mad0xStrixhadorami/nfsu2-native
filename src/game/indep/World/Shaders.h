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
uniform float uDebugPave;
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
            glass += vec3(1.0, 0.93, 0.78) * pow(max(dot(Rg, vec3(0.64, 0.46, 0.66)), 0.0), 160.0) * (1.2 + 3.0 * F) * step(0.5, uDay);
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
uniform vec3 uLamp; // x headlights, y brake pedal, z reverse (0..1)
uniform float uGlass; // 1 while the translucent lamp covers are drawn
uniform float uDay;   // 1 in daylight: the lamps burn less hot against the bright scene
varying float vAO;
varying vec4 vColor;
varying vec2 vUV;
varying vec3 vNormal;
varying vec3 vPos;
varying float vGloss;
varying float vDist;

// A procedural environment: night sky with a sodium horizon glow, rows of distant city lights and a couple of soft
// light boxes, so the clear coat has something to reflect (the original uses an environment-map texture).
uniform float uStage;       // 1 in the menu showroom (ISSUES #304): studio strip lights instead of the night city
uniform vec3 uStageA, uStageB;
vec3 studioEnv(vec3 R) {
    float h = R.z;
    vec3 col = mix(vec3(0.018, 0.020, 0.028), vec3(0.075, 0.09, 0.13), smoothstep(-0.2, 0.95, h));
    col += vec3(1.0, 0.97, 0.92) * smoothstep(0.50, 0.88, h) * exp(-pow(R.y * 4.2, 2.0)) * 2.0;            // overhead strip softbox
    float a = atan(R.y, R.x);
    float band = smoothstep(-0.12, 0.22, h) * (1.0 - smoothstep(0.55, 0.92, h));
    col += uStageA * exp(-pow((a - 1.25) * 4.2, 2.0)) * band * 2.4;                                           // tall panel, one side
    col += uStageB * exp(-pow((a + 1.25) * 4.2, 2.0)) * band * 2.4;                                           // and the other
    col += mix(uStageA, vec3(1.0), 0.35) * pow(max(0.0, dot(R, normalize(vec3(-1.0, 0.15, 0.35)))), 24.0) * 1.5; // back light
    col += vec3(1.0, 0.95, 0.9) * pow(max(0.0, dot(R, normalize(vec3(0.7, -0.1, 0.25)))), 40.0) * 0.8;       // front fill
    return col;
}
vec3 environment(vec3 R) {
    if (uStage > 0.5)
        return studioEnv(R);
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
            colG = refl * (0.35 + 0.65 * fr) + tint * 0.10 + vec3(1.0, 0.96, 0.82) * uLamp.x * 0.35;
            aG = clamp(0.16 + 0.55 * fr + 0.10 * uLamp.x, 0.0, 0.8);
        } else {
            colG = tint * (0.30 + 1.1 * uLamp.y) + refl * fr * 0.8;
            aG = clamp(0.50 + 0.30 * fr, 0.0, 0.9);
        }
        float fg = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
        gl_FragColor = vec4(mix(colG, uFogColor, fg * fg * (3.0 - 2.0 * fg)), aG);
        return;
    }
    if (vGloss > 9.5 && vGloss < 10.5) {
        // the centre brake light: the tail lens treatment
        vec3 Nc = normalize(vNormal);
        vec3 Vc = normalize(uCam - vPos);
        float core = pow(max(dot(Nc, Vc), 0.0), 1.5);
        vec3 hotc = mix(vec3(0.85, 0.05, 0.03), vec3(1.0, 0.11, 0.06), uLamp.y) * (1.0 + 0.45 * uLamp.y) * (0.65 + 0.55 * core);
        vec3 colc = mix(clamp(vColor.rgb, 0.0, 1.0) * 0.30, hotc, clamp(0.30 + 0.70 * uLamp.y, 0.0, 1.0));
        float fc = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
        gl_FragColor = vec4(mix(colc, uFogColor, fc * fc * (3.0 - 2.0 * fc)), 1.0);
        return;
    }
    if (vGloss > 5.5) {
        // ---- lamp lenses (ISSUES #284): 6 headlight, 7 tail / brake light. Emissive; the brake pedal drives the tail lamp
        // from a dim red to a hot lens, headlights burn white-warm; a clear-coat glint keeps the glass look. -----------------
        vec3 Nl = normalize(vNormal);
        vec3 Vl = normalize(uCam - vPos);
        float NdVl = max(dot(Nl, Vl), 0.0);
        vec3 lens = clamp(vColor.rgb, 0.0, 1.0);
        vec3 col;
        if (vGloss < 6.5) {
            float on = uLamp.x;
            vec3 hot = vec3(1.0, 0.96, 0.80) * mix(1.9 + 0.5 * NdVl, 0.62 + 0.42 * NdVl, uDay);
            col = mix(lens * (0.35 + 0.35 * NdVl), hot, on);
        } else {
            float on = clamp(0.30 + 0.70 * uLamp.y, 0.0, 1.0); // tail lamps always burn, the brake pedal drives them hard
            float core = pow(NdVl, 1.5);
            vec3 hot = mix(vec3(0.85, 0.05, 0.03), vec3(1.0, 0.11, 0.06), uLamp.y) * (1.0 + 0.45 * uLamp.y) * (0.65 + 0.55 * core);
            hot += vec3(0.0, 0.10, 0.04) * uLamp.y * pow(core, 3.0); // a hotter centre
            col = mix(lens * 0.30, hot, on);
        }
        col += vec3(pow(1.0 - NdVl, 4.0) * 0.12) + environment(reflect(-Vl, Nl)) * 0.10; // glassy rim light
        float fl = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
        gl_FragColor = vec4(mix(col, uFogColor, fl * fl * (3.0 - 2.0 * fl)), 1.0);
        return;
    }
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
        gl_FragColor = vec4(mix(col, uFogColor, fw * fw * (3.0 - 2.0 * fw)), (vGloss < 3.5) ? 1.0 : tex.a); // the tyre and the brake disc keep their alpha-tested (round) outline
        return;
    }
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    bool glass = vGloss > 1.5;
    // base lighting: hemisphere + sun, with a wrap term so the shadow side stays rich
    float hemi = 0.26 + 0.30 * (0.5 + 0.5 * N.z);
    float wrap = clamp((dot(N, L) + 0.35) / 1.35, 0.0, 1.0);
    vec3 lit = base.rgb * (hemi + 0.62 * wrap) * (1.0 - 0.30 * uStage);
    // clear coat: Fresnel-weighted environment reflection and two specular lobes (tight sun + broad sheen)
    vec3 R = reflect(-V, N);
    float F0 = (glass ? 0.085 : 0.045) + 0.10 * uStage; // the showroom paint is glossier: more of the studio lights shows
    float fres = F0 + (1.0 - F0) * pow(1.0 - NdV, 5.0);
    float reflectivity = glass ? 1.0 : 0.85;
    vec3 env = environment(R);
    vec3 H = normalize(L + V);
    float NdH = max(dot(N, H), 0.0);
    float spec = pow(NdH, glass ? 260.0 : 140.0) * 1.1 + pow(NdH, 18.0) * 0.12;
    // a metallic-flake glint that moves with the view (paint only)
    float flake = glass ? 0.0 : pow(max(0.0, sin(dot(vPos, vec3(61.0, 47.0, 53.0)) + NdV * 7.0)), 40.0) * 0.06 * (0.3 + NdL);
    vec3 col = lit * (1.0 - fres * reflectivity * 0.65) + env * (1.0 + 0.9 * uStage) * fres * reflectivity + vec3(spec) * (0.4 + 0.6 * NdL) + base.rgb * flake;
    if (glass) col = col * 0.55 + env * 0.18; // tinted glass picks up some of the surroundings even head-on
    float f = clamp((vDist - uFogStart) / max(uFogEnd - uFogStart, 1.0), 0.0, 1.0);
    col = mix(col, uFogColor, f * f * (3.0 - 2.0 * f));
    gl_FragColor = vec4(col, base.a);
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
uniform vec3 uSun;
uniform vec3 uHorizon;
uniform float uTime;
uniform float uDay;
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
    float day = step(0.5, uDay);
    // --- atmosphere ---
    vec3 zenith = mix(vec3(0.05, 0.10, 0.26), vec3(0.09, 0.27, 0.66), day);
    vec3 mid = mix(vec3(0.12, 0.20, 0.38), vec3(0.28, 0.56, 0.92), day);
    float e = clamp(el, 0.0, 1.0);
    vec3 haze = uHorizon * vec3(0.94, 0.95, 0.97);
    col = mix(haze, mid, smoothstep(0.0, 0.17, e));
    col = mix(col, zenith, smoothstep(0.10, 0.80, e));
    col = mix(col, haze, (1.0 - smoothstep(-0.05, 0.0, el)));     // below the horizon: the haze
    // low sun scatter: a warm band around the sun's azimuth near the horizon, and a Mie halo around the sun itself
    float hz = exp(-e * 7.0);
    col += vec3(1.0, 0.80, 0.55) * pow(sd, 5.0) * (0.10 + 0.22 * hz) * day;
    col += vec3(1.0, 0.92, 0.78) * pow(sd, 48.0) * 0.55 * day;
    col += vec3(1.0, 0.97, 0.90) * pow(sd, 700.0) * 2.4 * day;          // the glare
    float disc = smoothstep(0.99955, 0.99975, sd);
    col = mix(col, vec3(1.6, 1.5, 1.3), disc * day);
    // --- night: stars and a moon ---
    if (day < 0.5) {
        vec2 sp = d.xy / max(d.z + 0.35, 0.2) * 70.0;
        float st = h21(floor(sp));
        float tw = 0.6 + 0.4 * sin(uTime * (1.5 + st * 4.0) + st * 50.0);
        col += vec3(0.9, 0.95, 1.0) * step(0.985, st) * tw * smoothstep(0.05, 0.4, el) * 0.8;
        col = mix(col, vec3(0.85, 0.9, 1.0), smoothstep(0.9993, 0.9996, sd) * 0.9);
        col += vec3(0.30, 0.38, 0.55) * pow(sd, 90.0) * 0.5;
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
        vec3 lit = mix(vec3(1.02, 0.99, 0.94), vec3(1.0, 0.86, 0.68), (1.0 - day) * 0.0);
        vec3 shadow = mix(vec3(0.46, 0.54, 0.70), vec3(0.20, 0.24, 0.34), 1.0 - day);
        vec3 cc = mix(shadow, lit, clamp(shade, 0.0, 1.0)) * mix(0.22, 1.0, day);
        // bright rims where the cloud is thin and faces the sun (silver lining)
        float rim = (1.0 - dens) * smoothstep(0.0, 0.5, n - cover + 0.2) * pow(sd, 6.0);
        cc += vec3(1.0, 0.95, 0.85) * rim * 1.2 * day;
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
} // namespace Shaders
