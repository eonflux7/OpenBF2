// [INFERRED] console placement; the retail rows belong to ModuleGeom's initializer list.
#ifdef BF2_IMAGE_RENDDX9

#include "QuadPatch.h"
#include "Terrain.h"
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/PluginEntry.h"
#include "../Core/ConsoleObjectMacro.h"
#include "../Core/ConsoleObjects.h"
#if defined(__APPLE__)
#include "../Platform/X87Compat.h"
#endif

namespace dice {
namespace hfe {
namespace geom {
namespace {

const char kTerrainCullerConsoleTag[] = "geom.terrainCuller";

// PE 0x100B0CFE.
TerrainCuller* consoleTerrainCuller()
{
    return g_Terrain ? static_cast<Terrain*>(g_Terrain)->m_culler : 0;
}

// PE 0x100E3400. Shipped defect: the initial node is dereferenced without a null check.
void RD9_THUNK updateCullerBaseCells(TerrainCuller* self RD9_THUNK_EDX, TerrainCullerNode* node)
{
    do
    {
        if (node->bounds.dirty)
            node->bounds.refresh();
#if defined(__APPLE__)
        // [CHOSEN, not recovered] FUCOM compares the integer without a binary32 narrowing.
        node->fileByteEE = bf2x87::wide(node->bounds.extent.x) == bf2x87::wide(self->param14);
#else
        node->fileByteEE = node->bounds.extent.x == self->param14;
#endif
        if (node->child[0])
            updateCullerBaseCells(self RD9_THUNK_ARG, node->child[0]);
        if (node->child[1])
            updateCullerBaseCells(self RD9_THUNK_ARG, node->child[1]);
        if (node->child[2])
            updateCullerBaseCells(self RD9_THUNK_ARG, node->child[2]);
        node = node->child[3];
    } while (node);
}

// PE 0x100E3480.
void RD9_THUNK setCullerBaseCellSize(TerrainCuller* self RD9_THUNK_EDX, int value)
{
    if (value == self->param14 || value < self->param10)
        return;
    Terrain* const owner = self->terrain;
#if defined(__APPLE__)
    // [CHOSEN, not recovered] Keep the retained x87 product and FCOM unordered result.
    const bf2x87::Real maximum = bf2x87::mul(bf2x87::wide(owner->m_patchSize),
                                           bf2x87::wide(owner->m_primaryWorldScale.x));
    if (bf2x87::less(maximum, bf2x87::wide(value)))
        return;
#else
    if (owner->m_patchSize * owner->m_primaryWorldScale.x < value)
        return;
#endif
    if (value != 512 && value != 256 && value != 128
        && value != 64 && value != 32 && value != 16)
        return;
    TerrainCullerNode* const nodes = self->nodes;
    self->param14 = value;
    updateCullerBaseCells(self RD9_THUNK_ARG, nodes);
}

// The getter (`mov al, [ecx+0x78]`, versionFlag78) is read inline by the useStitchedLods binding below.
// PE 0x100E2FB0, 0x100E2FE0: the setter, then the getter.
void RD9_THUNK setCullerStitchedLods(TerrainCuller* self RD9_THUNK_EDX, bool value)
{
    Terrain* const owner = self->terrain;
    if (owner->m_rawHasVersion >= 1 && owner->m_rawVersion >= 0x1A)
        self->versionFlag78 = value;
}

// Console setter adapters keep the executable's repeated owner lookup at execution time.
void consoleBaseCellSize(int value) { setCullerBaseCellSize(consoleTerrainCuller() RD9_THUNK_ARG, value); }
void consoleStitchedLods(bool value) { setCullerStitchedLods(consoleTerrainCuller() RD9_THUNK_ARG, value); }

BF2_CONSOLE_OBJECT("terrainCuller", 2, 0)

// [RENDER] lodSwitchDistance (PE 0x100B6AB8) and lod0SwitchStart (PE 0x100B6C15) are gone: the
// detail-shading distance and the roads' view distance and fade they set are (render milestone 10).
// PE 0x100B7124 / 0x100B721F.
// PE 0x100B7066, 0x100B70D7
BF2_CONSOLE_FIELD(detailCullMaxLevel, int, consoleTerrainCuller(), param68, kTerrainCullerConsoleTag)
// PE 0x100B731A.
// PE 0x100B725C, 0x100B72CD
BF2_CONSOLE_PROPERTY_FN(baseCellSize, int, consoleTerrainCuller() != 0,
                        consoleTerrainCuller()->param14, consoleBaseCellSize, kTerrainCullerConsoleTag)
// PE 0x100B741A / 0x100B7515 / 0x100B761C.
// [RENDER] lod0SwitchStop, lod1SwitchStart, lod1SwitchStop, lod2Start, cullerType and
// updateLodLevel are gone with the terrain LODs; see TerrainCuller in QuadPatch.h.
// PE 0x100B7457, 0x100B74C8
BF2_CONSOLE_FIELD(checkInside, bool, consoleTerrainCuller(), flag71, kTerrainCullerConsoleTag)
// PE 0x100B7552, 0x100B75CF
BF2_CONSOLE_PROPERTY_FN(useStitchedLods, bool, consoleTerrainCuller() != 0,
                        consoleTerrainCuller()->versionFlag78 != 0, consoleStitchedLods, kTerrainCullerConsoleTag)

} // anonymous namespace

bool registerTerrainCullerConsoleObjects()
{
    return mainConsoleObjects->registerConsoleObjects(consoleBindingList(kTerrainCullerConsoleTag));
}

bool unregisterTerrainCullerConsoleObjects()
{
    return mainConsoleObjects->unregisterConsoleObjects(consoleBindingList(kTerrainCullerConsoleTag));
}

} // namespace geom
} // namespace hfe
} // namespace dice
#endif // BF2_IMAGE_RENDDX9
