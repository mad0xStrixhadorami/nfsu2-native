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

// ---- Cars: per-pixel paint with a clear coat, a procedural night-city environment and Fresnel -------------------------------
inline const char *kCarVS = R"(#version 120
attribute vec3 aNormal;
attribute float aGloss;
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
