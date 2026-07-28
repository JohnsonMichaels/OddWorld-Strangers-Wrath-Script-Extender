// SWSE Graphics - sharpening pass (OpenGL fragment shader, GLSL 1.20 era to
// match the game's Cg/GL2 pipeline). SWSE binds the captured 3D-scene color
// texture as uScene and runs this full-screen. UI/HUD are drawn by the game
// AFTER this pass, so they are never affected.
#version 120

uniform sampler2D uScene;    // captured scene color
uniform vec2  uTexel;        // 1.0 / render size
uniform float uStrength;     // from the Home overlay (graphics.json: sharpen.strength)

void main() {
    vec2 uv = gl_TexCoord[0].xy;
    vec3 c  = texture2D(uScene, uv).rgb;
    // 4-neighbour unsharp mask
    vec3 n  = texture2D(uScene, uv + vec2(0.0,  uTexel.y)).rgb
            + texture2D(uScene, uv + vec2(0.0, -uTexel.y)).rgb
            + texture2D(uScene, uv + vec2( uTexel.x, 0.0)).rgb
            + texture2D(uScene, uv + vec2(-uTexel.x, 0.0)).rgb;
    vec3 sharp = c + (c * 4.0 - n) * uStrength;
    gl_FragColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
