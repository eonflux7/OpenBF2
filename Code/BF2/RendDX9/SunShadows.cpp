// [CHOSEN, not recovered] Sun cascaded shadow maps (BF2_SUN_SHADOWS). Not part of retail BF2: the
// prototype renderer replaces BF2's per-object shadow maps with four sun cascades that feed the
// screen-space sun mask (FSQuadDrawer RaShadowMask). See RendDX9Base.h for the contract.
//
// Each cascade covers a slice of the camera's view depth with a bounding sphere, so its size does
// not change while the camera turns; its centre is snapped to whole texels across the light, so
// static shadows do not crawl while the camera moves. The atlas stores the light-space depth of
// the nearest caster (0 = the cascade's sun-side bound, 1 = its far side) as R32F; the mask does
// the comparison and the filtering.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "../RendMisc/IRenderView.h"
#include "../Math/Frustum.h"
#include "../Math/Mat4.h"
#include "../Geom/XpLightManager.h"
#include "../Geom/Terrain.h"
#include "../Geom/ObjectDrawer.h"
#include "../Geom/Overgrowth/OverGrowth.h"
#include "../Game/Objects/Visual/LightSource.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dice {
namespace hfe {

namespace geom { extern LightManager* g_LightManager; }

namespace dx9rend {

extern void* g_rendererBase;
extern void* g_OverGrowth;
unsigned int raHashString(const std::string& s);

namespace {

const unsigned int kCascades = 4;
const DWORD kShadowAtlasSampler = 13;

IDirect3DTexture9* g_atlas = 0;          // R32F, 2 x 2 cascades
IDirect3DSurface9* g_atlasDepth = 0;     // D24S8, standard (not reversed) depth
unsigned int g_cascadeSize = 0;

float envFloat(const char* name, float fallback)
{
    const char* const v = std::getenv(name);
    if (!v || !*v)
        return fallback;
    const float f = static_cast<float>(std::atof(v));
    return f > 0.0f ? f : fallback;
}

// The view depth the cascades reach, and the cascade resolution.
float shadowDistance()
{
    static const float d = envFloat("BF2_CSM_DISTANCE", 400.0f);
    return d;
}

unsigned int cascadeResolution()
{
    static const unsigned int r = static_cast<unsigned int>(envFloat("BF2_CSM_RES", 2048.0f));
    return r;
}

// How far toward the sun past a cascade's sphere casters are still drawn (world units).
float casterReach()
{
    static const float d = envFloat("BF2_CSM_CASTER_REACH", 500.0f);
    return d;
}

IDirect3DDevice9* device()
{
    return static_cast<RendDX9Base*>(g_rendererBase)->device;
}

bool ensureAtlas()
{
    const unsigned int size = cascadeResolution();
    if (g_atlas && g_cascadeSize == size)
        return true;
    rendererSunShadowsRelease();
    HRESULT hr = device()->CreateTexture(size * 2, size * 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                         D3DPOOL_DEFAULT, &g_atlas, 0);
    if (hr >= 0)
        hr = device()->CreateDepthStencilSurface(size * 2, size * 2, D3DFMT_D24S8,
                                                 D3DMULTISAMPLE_NONE, 0, TRUE, &g_atlasDepth, 0);
    std::fprintf(stderr, "[SUNSHADOWS] atlas %ux%u R32F (4 cascades of %u): hr=0x%08lX\n",
                 size * 2, size * 2, size, static_cast<unsigned long>(hr));
    if (hr < 0)
    {
        rendererSunShadowsRelease();
        return false;
    }
    g_cascadeSize = size;
    return true;
}

struct Vec3f
{
    float x, y, z;
};

inline float dot(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3f cross(const Vec3f& a, const Vec3f& b)
{
    const Vec3f c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return c;
}
inline Vec3f normalize(const Vec3f& a)
{
    const float l = std::sqrt(dot(a, a));
    const Vec3f n = { a.x / l, a.y / l, a.z / l };
    return n;
}
inline Vec3f madd(const Vec3f& a, const Vec3f& b, float s)
{
    const Vec3f c = { a.x + b.x * s, a.y + b.y * s, a.z + b.z * s };
    return c;
}

struct Cascade
{
    float lightX, lightY, lightZ;   // sphere centre in light coordinates (texel-snapped across)
    float radius;
    float nearZ, rangeZ;            // light depth: [nearZ, nearZ + rangeZ]
    float splitFar;                 // view depth the cascade ends at
    Vec3f centre;                   // world sphere centre
};

} // namespace

bool rendererSunShadowsEnabled()
{
    static const bool enabled = [] {
        const char* v = std::getenv("BF2_SUN_SHADOWS");
        const bool on = v && *v && *v != '0';
        std::fprintf(stderr, "[SUNSHADOWS] %s\n", on ? "on" : "off");
        return on;
    }();
    return enabled && rendererDepthPrepassEnabled();
}

void rendererSunShadowsRelease()
{
    if (g_atlas && g_rendererBase)
        device()->SetTexture(kShadowAtlasSampler, 0);
    if (g_atlas)
        g_atlas->Release();
    g_atlas = 0;
    if (g_atlasDepth)
        g_atlasDepth->Release();
    g_atlasDepth = 0;
    g_cascadeSize = 0;
}

bool rendererSunShadowsRender(float time, float* constants)
{
    if (!rendererSunShadowsEnabled() || !geom::g_LightManager)
        return false;
    world::LightSource* const sky = geom::g_LightManager->getSkyLight();
    if (!sky)
        return false;
    const Vec3& d = sky->getDirection();          // the direction the sunlight travels
    Vec3f forward = { d.x, d.y, d.z };
    if (dot(forward, forward) < 1e-8f)
        return false;
    forward = normalize(forward);
    if (forward.y > -0.01f)                       // the sun is at or below the horizon
        return false;
    if (!ensureAtlas())
        return false;

    // The light basis: forward along the sunlight, right and up across it.
    const Vec3f worldUp = { 0.0f, 1.0f, 0.0f };
    const Vec3f worldZ = { 0.0f, 0.0f, 1.0f };
    const Vec3f right = normalize(cross(std::fabs(forward.y) > 0.99f ? worldZ : worldUp, forward));
    const Vec3f up = cross(forward, right);

    // The camera: its world transform rows and the projection's tangents.
    IRenderView* const view = rendererGetRenderView();
    const float* const c = view->getViewMatrixPtr()->m;
    const float* const p = view->getProjectionMatrixPtr()->m;
    const Vec3f cameraPos = { c[12], c[13], c[14] };
    const Vec3f cameraFwd = { c[8], c[9], c[10] };
    const float tanX = 1.0f / p[0];
    const float tanY = 1.0f / p[5];
    const float slope2 = tanX * tanX + tanY * tanY;   // squared corner slope
    float nearDist = view->getFrustumPtr()->getNearDist();
    if (!(nearDist > 0.05f))
        nearDist = 0.05f;
    const float farDist = shadowDistance();

    // Practical split scheme: a blend of logarithmic and uniform splits.
    const float lambda = 0.75f;
    const unsigned int size = g_cascadeSize;
    Cascade cascades[kCascades];
    float splitNear = nearDist;
    for (unsigned int i = 0; i < kCascades; ++i)
    {
        const float t = static_cast<float>(i + 1) / kCascades;
        const float logSplit = nearDist * std::pow(farDist / nearDist, t);
        const float uniSplit = nearDist + (farDist - nearDist) * t;
        const float splitFar = lambda * logSplit + (1.0f - lambda) * uniSplit;

        // The smallest sphere around the slice's eight corners: its centre lies on the view axis.
        float centreDepth = 0.5f * (splitNear + splitFar) * (1.0f + slope2);
        if (centreDepth > splitFar)
            centreDepth = splitFar;
        const float farOff = splitFar - centreDepth;
        float radius = std::sqrt(farOff * farOff + splitFar * splitFar * slope2);
        radius = std::ceil(radius * 16.0f) / 16.0f;     // steady across float noise

        const Vec3f centre = madd(cameraPos, cameraFwd, centreDepth);
        const float texel = 2.0f * radius / static_cast<float>(size);
        Cascade& k = cascades[i];
        k.lightX = std::floor(dot(centre, right) / texel) * texel;
        k.lightY = std::floor(dot(centre, up) / texel) * texel;
        k.lightZ = dot(centre, forward);
        k.radius = radius;
        k.nearZ = k.lightZ - radius - casterReach();
        k.rangeZ = 2.0f * radius + casterReach();
        k.splitFar = splitFar;
        k.centre = madd(madd(madd(Vec3f(), right, k.lightX), up, k.lightY), forward, k.lightZ);
        splitNear = splitFar;
    }

    // The caster volume: the light-space box around every cascade.
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (unsigned int i = 0; i < kCascades; ++i)
    {
        const Cascade& k = cascades[i];
        minX = std::fmin(minX, k.lightX - k.radius);
        maxX = std::fmax(maxX, k.lightX + k.radius);
        minY = std::fmin(minY, k.lightY - k.radius);
        maxY = std::fmax(maxY, k.lightY + k.radius);
        minZ = std::fmin(minZ, k.nearZ);
        maxZ = std::fmax(maxZ, k.lightZ + k.radius);
    }
    Mat4 basis = Mat4::getIdentity();
    const Vec3f volumeOrigin = madd(madd(Vec3f(), right, 0.5f * (minX + maxX)), up, 0.5f * (minY + maxY));
    basis.m[0] = right.x;    basis.m[1] = right.y;    basis.m[2] = right.z;
    basis.m[4] = up.x;       basis.m[5] = up.y;       basis.m[6] = up.z;
    basis.m[8] = forward.x;  basis.m[9] = forward.y;  basis.m[10] = forward.z;
    basis.m[12] = volumeOrigin.x;
    basis.m[13] = volumeOrigin.y;
    basis.m[14] = volumeOrigin.z;
    Frustum volume;
    volume.setOrtho(maxX - minX, maxY - minY, minZ, maxZ, basis);

    // Casters: the scene's own submission against the volume, then the terrain's nodes.
    rendererBeginCasterCapture(&volume);
    geom::objectDrawerSubmitShadowCasters(&volume, time);
    if (g_OverGrowth)
        static_cast<geom::OverGrowth*>(g_OverGrowth)->submitShadowCasters(view, volume);
    rendererEndCasterCapture();
    geom::Terrain* const terrain = static_cast<geom::Terrain*>(geom::g_Terrain);
    if (terrain)
        terrain->gatherShadowCasters(volume);

    // Render the cascades.
    IDirect3DSurface9* savedTarget = stateManagerGetRenderTarget(0);
    IDirect3DSurface9* savedDepth = stateManagerGetDepthStencilSurface();
    if (!savedTarget)
        savedTarget = rendererSceneColorSurface();
    if (!savedDepth)
        savedDepth = rendererDepthStencilSurface();
    if (savedTarget)
        savedTarget->AddRef();
    if (savedDepth)
        savedDepth->AddRef();
    D3DVIEWPORT9 savedViewport;
    device()->GetViewport(&savedViewport);
    device()->SetTexture(kShadowAtlasSampler, 0);

    IDirect3DSurface9* target = 0;
    g_atlas->GetSurfaceLevel(0, &target);
    stateManagerSetRenderTarget(0, target);
    stateManagerSetDepthStencilSurface(g_atlasDepth);
    stateManagerMarkStandardDepth(true);
    flushRenderTargetsSeam();
    stateManagerClear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFFFFFFFFu, 1.0f, 0);

    static const unsigned int key = raHashString(std::string("RaShadowDepth"));
    const float half = 0.5f * static_cast<float>(size);
    for (unsigned int i = 0; i < kCascades; ++i)
    {
        const Cascade& k = cascades[i];
        const DWORD originX = (i & 1u) ? size : 0u;
        const DWORD originY = (i & 2u) ? size : 0u;
        const D3DVIEWPORT9 viewport = { originX, originY, size, size, 0.0f, 1.0f };
        device()->SetViewport(&viewport);

        // World to cascade clip space (row vectors, as the effects multiply).
        const float r = k.radius;
        const float zr = k.rangeZ;
        const float viewProjection[16] = {
            right.x / r, up.x / r, forward.x / zr, 0.0f,
            right.y / r, up.y / r, forward.y / zr, 0.0f,
            right.z / r, up.z / r, forward.z / zr, 0.0f,
            -k.lightX / r, -k.lightY / r, -k.nearZ / zr, 1.0f };
        ShadowCascadeCull cull;
        cull.right[0] = right.x;     cull.right[1] = right.y;     cull.right[2] = right.z;
        cull.up[0] = up.x;           cull.up[1] = up.y;           cull.up[2] = up.z;
        cull.forward[0] = forward.x; cull.forward[1] = forward.y; cull.forward[2] = forward.z;
        cull.centre[0] = k.centre.x; cull.centre[1] = k.centre.y; cull.centre[2] = k.centre.z;
        cull.radius = r;

        if (terrain)
            terrain->drawShadowCasters(viewProjection, cull);
        renderQueuesDrawShadowCasters(key, viewProjection, cull);

        // The mask's rows: world position to atlas texel coordinates (texel n at n, the D3D9
        // pixel-centre convention) and light depth. Rows 4 + 3i .. 6 + 3i.
        float* const row = constants + 4 * (4 + 3 * i);
        const float s = half / r;
        row[0] = right.x * s;  row[1] = right.y * s;  row[2] = right.z * s;
        row[3] = half - k.lightX * s + static_cast<float>(originX);
        row[4] = -up.x * s;    row[5] = -up.y * s;    row[6] = -up.z * s;
        row[7] = half + k.lightY * s + static_cast<float>(originY);
        row[8] = forward.x / zr; row[9] = forward.y / zr; row[10] = forward.z / zr;
        row[11] = -k.nearZ / zr;

        constants[4 * 16 + i] = k.splitFar;                     // RASM[16]: cascade ends
        constants[4 * 18 + i] = 2.0f * r / static_cast<float>(size);   // RASM[18]: texel size
        constants[4 * 19 + i] = 1.0f / zr;                      // RASM[19]: depth per metre
    }
    // RASM[17]: toward the sun, and the view depth the blend to baked sunlight starts at.
    constants[4 * 17 + 0] = -forward.x;
    constants[4 * 17 + 1] = -forward.y;
    constants[4 * 17 + 2] = -forward.z;
    constants[4 * 17 + 3] = 0.9f * cascades[kCascades - 1].splitFar;

    stateManagerSetRenderTarget(0, savedTarget);
    stateManagerSetDepthStencilSurface(savedDepth);
    flushRenderTargetsSeam();
    device()->SetViewport(&savedViewport);
    if (target)
        target->Release();
    if (savedTarget)
        savedTarget->Release();
    if (savedDepth)
        savedDepth->Release();

    device()->SetTexture(kShadowAtlasSampler, g_atlas);
    device()->SetSamplerState(kShadowAtlasSampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device()->SetSamplerState(kShadowAtlasSampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device()->SetSamplerState(kShadowAtlasSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device()->SetSamplerState(kShadowAtlasSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device()->SetSamplerState(kShadowAtlasSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device()->SetSamplerState(kShadowAtlasSampler, D3DSAMP_SRGBTEXTURE, FALSE);

    static bool reported = false;
    if (!reported)
    {
        reported = true;
        std::fprintf(stderr, "[SUNSHADOWS] first frame: %u mesh casters, splits %.1f %.1f %.1f %.1f, "
                     "radii %.1f %.1f %.1f %.1f\n",
                     static_cast<unsigned int>(rendererShadowCasters().size()),
                     cascades[0].splitFar, cascades[1].splitFar, cascades[2].splitFar,
                     cascades[3].splitFar, cascades[0].radius, cascades[1].radius,
                     cascades[2].radius, cascades[3].radius);
    }
    return true;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
