// SWSE Graphics - Screen-Space Global Illumination (SSGI).
// GLSL 1.20 to match the game's GL2/Cg-era pipeline.
//
// This is the "RTGI look" pass: it approximates one bounce of indirect light by
// sampling the scene color buffer around each pixel, weighted by depth so light
// only bounces off nearby surfaces. It is screen-space (no real ray tracing) -
// the same technique behind Kenshi's "Dust" GI and ReShade RTGI.
//
// Requires depth (uDepth). SWSE binds uScene (scene color) + uDepth each frame.
// Runs on the 3D scene only, before the game draws HUD/menus.
#version 120

uniform sampler2D uScene;   // scene color
uniform sampler2D uDepth;   // scene depth (linearized to view space by SWSE)
uniform vec2  uTexel;       // 1.0 / render size
uniform float uIntensity;   // graphics.json: ssgi.intensity
uniform float uRadius;      // ssgi.radius (view-space units)
uniform int   uSamples;     // ssgi.samples

const float PI = 3.14159265;

float depthAt(vec2 uv) { return texture2D(uDepth, uv).r; }

void main() {
    vec2 uv = gl_TexCoord[0].xy;
    vec3 direct = texture2D(uScene, uv).rgb;
    float d0 = depthAt(uv);

    vec3 bounce = vec3(0.0);
    float total = 0.0;

    // spiral-sample the neighbourhood; gather color from surfaces at similar depth
    for (int i = 0; i < 64; i++) {
        if (i >= uSamples) break;
        float t = (float(i) + 0.5) / float(uSamples);
        float ang = t * PI * 2.0 * 4.0;          // 4 turns
        float rad = t * uRadius;
        vec2 off = vec2(cos(ang), sin(ang)) * rad * uTexel * 64.0;
        vec2 suv = uv + off;

        float dS = depthAt(suv);
        // occlusion/range weight: only accept samples on nearby surfaces
        float dz = abs(dS - d0);
        float w  = smoothstep(uRadius, 0.0, dz) * (1.0 - t); // nearer + closer depth = stronger
        bounce += texture2D(uScene, suv).rgb * w;
        total  += w;
    }
    if (total > 0.0) bounce /= total;

    // indirect light adds to the scene, modulated by the surface's own albedo
    vec3 gi = direct + direct * bounce * uIntensity;
    gl_FragColor = vec4(gi, 1.0);
}
