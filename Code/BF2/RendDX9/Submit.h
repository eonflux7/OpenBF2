// [RENDER] Draw submission (render milestone 9, docs/eon64/renderer.md). Not part of retail BF2.
//
// A program is one pass of one technique of a Ra effect, taken from the effect file once: its
// vertex and pixel shaders, its fixed render and sampler states, and where each effect parameter
// it reads lives in the shaders' registers and samplers. A draw then binds the program's states
// once per run of draws and per draw uploads only the constants the program reads, straight into
// the state manager's mirror. D3DX is not called while drawing.
//
// Parameter values still come from the Ra parameter holders: each holder applies its value to a
// sink with the ID3DXEffect shape, which turns it into register writes. So every holder kind
// works unchanged, and the effect files stay the one source of shaders and constant layout.
//
// The contract (renderer-pipeline.md, shader contract version 2): the passes drawn here set only
// constant render states, except the three a material decides, which the engine sets per draw from
// the parameters with semantic RaAlphaTest, RaAlphaRef and RaDepthWrite.
#pragma once

#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

namespace Ra { class RaShader; }
struct RenderQueueEntry;
struct SubmitProgram;

// renderer.submit: 1 draws the depth prepass and the sun casters through programs, 0 through
// D3DX (the A/B switch while milestone 9 is developed).
bool submitEnabled();
void submitSetEnabled(bool on);

// The program for `shader`'s technique `key`, pass 0, built on first use; null when the effect
// has no such technique or the pass cannot be drawn here (logged once).
SubmitProgram* submitProgram(Ra::RaShader* shader, unsigned int key);

// Binds `program`'s shaders and fixed states inside a state-manager bracket and uploads the
// effect-wide parameters for `entry` (the global set). submitEnd closes the bracket.
void submitBegin(SubmitProgram* program, RenderQueueEntry* entry);
void submitEnd(SubmitProgram* program);

// Sets the parameter with `semantic` (a float4x4, row-major) for the draws that follow; false when
// the program has no such parameter.
bool submitSetMatrix(SubmitProgram* program, const char* semantic, const float* matrix);
bool submitHasParameter(SubmitProgram* program, const char* semantic);

// Uploads `entry`'s material and object parameters and sets its material's states. Returns
// whether the draw writes depth (with the depth test on), for the prepass's filter.
bool submitEntry(SubmitProgram* program, RenderQueueEntry* entry);

// Drops every program (effects reset or reloaded).
void submitReset();

} // namespace dx9rend
} // namespace hfe
} // namespace dice
