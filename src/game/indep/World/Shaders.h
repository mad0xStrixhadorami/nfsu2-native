#pragma once
// GLSL 1.20 sources for the world renderer (ISSUES #263). They keep the fixed-function vertex arrays and matrices.
namespace Shaders {

// ---- World: textured, vertex-lit scenery with graded colour and distance fog -------------------------------------
inline const char *kWorldVS = R"(#version 120
varying vec4 vColor;
varying vec2 vUV;
varying float vDist;
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    vColor = gl_Color;
    vUV = gl_MultiTexCoord0.xy;
    vDist = length((gl_ModelViewMatrix * gl_Vertex).xyz);
}
)";

inline const char *kWorldFS = R"(#version 120
uniform sampler2D uTex;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uGrade;
varying vec4 vColor;
varying vec2 vUV;
varying float vDist;
void main() {
    vec4 t = texture2D(uTex, vUV);
    vec4 c = t * vColor; // the PS2's MODULATE (vertex colours were doubled on the CPU)
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
