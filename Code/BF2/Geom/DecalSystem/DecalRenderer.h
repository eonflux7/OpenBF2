// Batches active decal records and draws them with the Decals shader.
// [INFERRED] source placement; [CHOSEN, not recovered] file split.
// DecalManager owns this renderer. Each batch holds up to ten instance transforms.
// Keep opaque storage at +0x10 ([UNRESOLVED] dword), +0x14 ([UNRESOLVED] byte) and
// +0x80 ([UNRESOLVED] 16-byte region); the latter two unused regions have no known readers.

#ifndef BF2_GEOM_DECALSYSTEM_DECALRENDERER_H
#define BF2_GEOM_DECALSYSTEM_DECALRENDERER_H

#ifdef BF2_IMAGE_RENDDX9

#include "../../Math/Mat4.h"
#include "../../Math/Vec3.h"

#include <map>
#include <stddef.h>
#include <vector>

namespace dice {
namespace hfe {

namespace dx9rend {
class Effect;
class ShaderParameter;
class IRenderView;
struct EngineTexture;
}

namespace geom {

class DecalObject;
struct DecalRecord;

// The manager's client and active sets: every entry maps an object to itself (retail's
// std::map<DecalObject*, DecalObject*>, ordered by the raw pointer).
typedef std::map<DecalObject*, DecalObject*> DecalObjectMap;


// Batch grouped by texture. Holds at most ten object transforms.
// Vectors own their storage; object and record entries are borrowed pointers.
// [RENDER] Retail also grouped by kind (0 unshadowed, 1 and 2 receiving a directional shadow
// record) and by shadow map; the legacy sun shadow is gone, so every batch is retail's kind 0.

struct DecalBatch
{
    DecalBatch() : vertexCount(0), texture(0) {}

    // vertexCount = 0, texture = 0, then both vectors cleared, objects first.
    // PE 0x101AB200
    void reset()
    {
        vertexCount = 0;
        texture = 0;
        objects.clear();
        records.clear();
    }

    unsigned int                       vertexCount;
    dx9rend::EngineTexture*            texture;
    std::vector<DecalObject*>          objects;
    std::vector<DecalRecord*>          records;
};

class DecalRenderer
{
public:
    DecalRenderer();
    ~DecalRenderer();

    // (retn 8) - batch every changed, non-empty active DecalObject. DecalManager's listener
    // hands it a copy of the renderer's view and its active set.
    void collect(dx9rend::IRenderView& view, DecalObjectMap& instances);
    // (retn 4) - sort the batches, draw them, drop them. Answers false (and draws
    // nothing) when disabled or when there is no batch; true otherwise.
    bool render(dx9rend::IRenderView* view);

    // The three empty bodies, all `retn`. 0x101A9AA0 is the one DecalManager's clear calls
    // (0x10124B99); 0x101A9A80 / 0x101A9A90 have no reference. [INFERRED] names.
    void unused101() {}
    void unused102() {}
    void reset() {}

private:
    DecalRenderer(const DecalRenderer&);
    DecalRenderer& operator=(const DecalRenderer&);

    void addObject(dx9rend::IRenderView& view, DecalObject* object, DecalBatch& batch);
    void addRecord(DecalBatch& batch, DecalRecord* record, DecalObject* object,
                   int* objectIndex);
    void pushBatch(const DecalBatch& batch);
    void beginPass();
    bool drawBatch(dx9rend::IRenderView* view, const DecalBatch& batch);
    void setInstanceTransforms(const DecalBatch& batch);

    std::vector<DecalBatch>       m_batches;                  // +0x000
    unsigned int                  m_dword10;                  // +0x010  [UNRESOLVED]
    unsigned char                 m_byte14;                   // +0x014  [UNRESOLVED]
    unsigned char                 m_pad15[3];
    dx9rend::EngineTexture*       m_boundTexture;             // +0x018
    Vec4                          m_viewColumn;               // +0x020
    Mat4                          m_worldViewProjection;      // +0x030
    Vec4                          m_fadeDistanceAndInterval;  // +0x070
    unsigned char                 m_pad80[0x10];              // +0x080  [UNRESOLVED]
    bool                          m_enabled;                  // +0x090
    unsigned char                 m_pad91[3];
    void*                         m_vertexBuffer;             // +0x094
    dx9rend::Effect*              m_effect;                   // +0x098
    unsigned int                  m_vertexCursor;             // +0x09C
    const Mat4*                   m_instanceTransforms[10];   // +0x0A0
    dx9rend::ShaderParameter*     m_paramFadeDistanceAndInterval;   // +0x1B8
    dx9rend::ShaderParameter*     m_paramInstanceTransformations;   // +0x1BC
    dx9rend::ShaderParameter*     m_paramAmbientColor;              // +0x1CC
    dx9rend::ShaderParameter*     m_paramSunColor;                  // +0x1D0
    dx9rend::ShaderParameter*     m_paramSunDirection;              // +0x1D4
    dx9rend::ShaderParameter*     m_paramWorldViewProjection;       // +0x1D8
    dx9rend::ShaderParameter*     m_paramTexLayer0;                 // +0x1DC
    dx9rend::ShaderParameter*     m_paramFogRange;                  // +0x1E4
    dx9rend::ShaderParameter*     m_paramFogColor;                  // +0x1E8
    dx9rend::ShaderParameter*     m_paramWaterFogColor;             // +0x1EC
    dx9rend::ShaderParameter*     m_paramWaterFogRange;             // +0x1F0
    dx9rend::ShaderParameter*     m_paramCameraToWater;             // +0x1F4
    dx9rend::ShaderParameter*     m_paramWorldSpaceCamPos;          // +0x1F8
};

// The retail offsets stay beside the members; with std::vector members the object keeps neither
// the retail size (0x1FC) nor the offsets after its first vector.

} // namespace geom
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

#endif // BF2_GEOM_DECALSYSTEM_DECALRENDERER_H
