// SWSE shader reconnaissance.
//
// Dumps every shader program the game has created, so we can read how it
// transforms vertices. That single fact decides how foliage wind must be
// implemented:
//
//   * If vertex programs transform via FIXED-FUNCTION MATRIX STATE
//     (ARB "state.matrix.mvp", or GLSL ftransform()/gl_ModelViewProjectionMatrix)
//     then multiplying the modelview by an object-space shear before a foliage
//     draw bends the plant, with no shader rewrite at all.
//
//   * If they use their own uniform matrices, matrix state is ignored and the
//     displacement has to be injected into the shader source instead.
//
// Read-only: nothing is patched and no program is modified. ARB programs must
// be bound to be read back, so the previous binding is saved and restored.
#pragma once

// Request a dump. Serviced on the next frame, where a GL context is current.
// path may be NULL for the default (swse_shaders.txt next to the exe).
void SWSE_ShaderDumpRequest(const char* path);

// Called from the frame hook with the GL context current. Cheap no-op unless
// a dump was requested.
void SWSE_ShaderDumpService();

// Results of the last dump, for console reporting.
// glslPrograms/arbPrograms are counts; usesFixedMatrix is how many vertex
// programs referenced fixed-function matrix state (the deciding number).
void SWSE_ShaderDumpStats(int* glslPrograms, int* glslShaders,
                          int* arbPrograms, int* usesFixedMatrix,
                          int* done);
