// [RENDER] Sun cascaded shadow maps. Not part of retail BF2: the
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
#include "DrawStats.h"
#include "GpuTimers.h"
#include "../RendMisc/IRenderView.h"
#include "../Math/Frustum.h"
#include "../Math/Mat4.h"
#include "../Geom/XpLightManager.h"
#include "../Geom/Terrain.h"
#include "../Geom/ObjectDrawer.h"
#include "../Geom/Overgrowth/OverGrowth.h"
#include "../Game/Objects/Visual/LightSource.h"

#include <algorithm>
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

SunShadowSettings g_settings = { 400.0f, 2048u, 500.0f, 0.75f, 0.2f, 1.5f, 1.0f };
unsigned int g_cascadeDraws[kCascades] = {};   // draw calls per cascade, last frame rendered

// The renderer's running count of this frame's draw calls (indexed and not).
unsigned int frameDrawCalls()
{
    const unsigned char* const stats = static_cast<RendDX9Base*>(g_rendererBase)->renderStats;
    return *reinterpret_cast<const unsigned int*>(stats + 452)
        + *reinterpret_cast<const unsigned int*>(stats + 460);
}

IDirect3DDevice9* device()
{
    return static_cast<RendDX9Base*>(g_rendererBase)->device;
}

bool ensureAtlas()
{
    const unsigned int size = g_settings.resolution;
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
    float splitNear;                // view depth the cascade's slice starts at
    float splitFar;                 // view depth the cascade's slice ends at
    float coverFar;                 // view depth its sphere covers to (into the next slice)
};

// The casters that can shadow a receiver in the cascade's slice: the slice swept toward the sun.
// Across the light that is the 2D convex hull of the slice's eight corners; along it, from the
// projection's sun-side bound to the slice's farthest corner. The cascade's square (its bounding
// sphere) holds the hull, so nothing inside it is lost; what the hull leaves out is the ground
// beside and behind the view that only the square covers.
ShadowCascadeCull cascadeCull(const Cascade& k, const Vec3f& right, const Vec3f& up,
                              const Vec3f& forward, const Vec3f& cameraPos,
                              const Vec3f& cameraRight, const Vec3f& cameraUp,
                              const Vec3f& cameraFwd, float tanX, float tanY, float texel)
{
    // Receivers on the hull's edge filter over neighbouring texels; casters that close still count.
    // The receivers' filter reaches 2.5 texels; their normal offset adds to it.
    const float margin = (2.5f + g_settings.normalOffset) * texel;
    ShadowCascadeCull cull;
    cull.right[0] = right.x;     cull.right[1] = right.y;     cull.right[2] = right.z;
    cull.up[0] = up.x;           cull.up[1] = up.y;           cull.up[2] = up.z;
    cull.forward[0] = forward.x; cull.forward[1] = forward.y; cull.forward[2] = forward.z;

    float px[8], py[8];
    float maxZ = -1e30f;
    for (int i = 0; i < 8; ++i)
    {
        const float depth = (i & 4) ? k.coverFar : k.splitNear;
        const float sx = (i & 1) ? depth * tanX : -depth * tanX;
        const float sy = (i & 2) ? depth * tanY : -depth * tanY;
        const Vec3f p = madd(madd(madd(cameraPos, cameraFwd, depth), cameraRight, sx), cameraUp, sy);
        px[i] = dot(p, right);
        py[i] = dot(p, up);
        maxZ = std::fmax(maxZ, dot(p, forward));
    }
    cull.minZ = k.nearZ;
    cull.maxZ = maxZ;

    // Monotone chain, counter-clockwise; at most eight hull points from eight corners.
    int order[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    std::sort(order, order + 8, [&](int a, int b) {
        return px[a] < px[b] || (px[a] == px[b] && py[a] < py[b]);
    });
    const auto turn = [&](int o, int a, int b) {
        return (px[a] - px[o]) * (py[b] - py[o]) - (py[a] - py[o]) * (px[b] - px[o]);
    };
    int hull[16];
    int n = 0;
    for (int i = 0; i < 8; ++i)
    {
        while (n >= 2 && turn(hull[n - 2], hull[n - 1], order[i]) <= 0.0f)
            --n;
        hull[n++] = order[i];
    }
    for (int i = 6, lower = n + 1; i >= 0; --i)
    {
        while (n >= lower && turn(hull[n - 2], hull[n - 1], order[i]) <= 0.0f)
            --n;
        hull[n++] = order[i];
    }
    --n;   // the last point repeats the first

    cull.edgeCount = 0;
    for (int i = 0; i < n && cull.edgeCount < 8; ++i)
    {
        const int a = hull[i];
        const int b = hull[(i + 1) % n];
        const float ex = px[b] - px[a];
        const float ey = py[b] - py[a];
        const float len = std::sqrt(ex * ex + ey * ey);
        if (!(len > 1e-4f))
            continue;
        const float nx = ey / len;     // outward for a counter-clockwise hull
        const float ny = -ex / len;
        float* const edge = cull.edges[cull.edgeCount++];
        edge[0] = nx;
        edge[1] = ny;
        edge[2] = nx * px[a] + ny * py[a] + margin;
    }
    return cull;
}

} // namespace

SunShadowSettings& sunShadowSettings()
{
    return g_settings;
}

unsigned int sunShadowCascadeDraws(unsigned int cascade)
{
    return cascade < kCascades ? g_cascadeDraws[cascade] : 0;
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
    if (!geom::g_LightManager)
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
    const Vec3f cameraRight = { c[0], c[1], c[2] };
    const Vec3f cameraUp = { c[4], c[5], c[6] };
    const Vec3f cameraFwd = { c[8], c[9], c[10] };
    const float tanX = 1.0f / p[0];
    const float tanY = 1.0f / p[5];
    const float slope2 = tanX * tanX + tanY * tanY;   // squared corner slope
    float nearDist = view->getFrustumPtr()->getNearDist();
    if (!(nearDist > 0.05f))
        nearDist = 0.05f;
    const float farDist = g_settings.distance;

    // Practical split scheme: a blend of logarithmic and uniform splits. Each cascade but the last
    // also covers the first cascadeBlend of the next slice, where the mask blends the two.
    const float lambda = g_settings.splitLambda;
    const float blend = g_settings.cascadeBlend;
    const float reach = g_settings.casterReach;
    const auto splitDepth = [&](unsigned int n) {
        const float t = static_cast<float>(n) / kCascades;
        return lambda * nearDist * std::pow(farDist / nearDist, t)
            + (1.0f - lambda) * (nearDist + (farDist - nearDist) * t);
    };
    const unsigned int size = g_cascadeSize;
    Cascade cascades[kCascades];
    float splitNear = nearDist;
    for (unsigned int i = 0; i < kCascades; ++i)
    {
        const float splitFar = splitDepth(i + 1);
        const float coverFar = i + 1 < kCascades
            ? splitFar + blend * (splitDepth(i + 2) - splitFar)
            : splitFar;

        // The smallest sphere around the covered slice's eight corners: its centre lies on the
        // view axis.
        float centreDepth = 0.5f * (splitNear + coverFar) * (1.0f + slope2);
        if (centreDepth > coverFar)
            centreDepth = coverFar;
        const float farOff = coverFar - centreDepth;
        float radius = std::sqrt(farOff * farOff + coverFar * coverFar * slope2);
        radius = std::ceil(radius * 16.0f) / 16.0f;     // steady across float noise

        const Vec3f centre = madd(cameraPos, cameraFwd, centreDepth);
        const float texel = 2.0f * radius / static_cast<float>(size);
        Cascade& k = cascades[i];
        k.lightX = std::floor(dot(centre, right) / texel) * texel;
        k.lightY = std::floor(dot(centre, up) / texel) * texel;
        k.lightZ = dot(centre, forward);
        k.radius = radius;
        k.nearZ = k.lightZ - radius - reach;
        k.rangeZ = 2.0f * radius + reach;
        k.splitNear = splitNear;
        k.splitFar = splitFar;
        k.coverFar = coverFar;
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
    if (drawStatsOn())   // [DEV] BF2_DRAW_STATS: captured casters, and those without bounds
    {
        const std::vector<ShadowCaster>& casters = rendererShadowCasters();
        unsigned int unbounded = 0;
        for (size_t i = 0; i < casters.size(); ++i)
            unbounded += casters[i].sphere[3] < 0.0f ? 1u : 0u;
        drawStatsCount("sun casters", static_cast<unsigned int>(casters.size()));
        drawStatsCount("sun casters unbounded", unbounded);
    }
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
    gpuTimerMark("sun clear");   // [DEV] BF2_GPU_TIMERS
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
        const ShadowCascadeCull cull = cascadeCull(k, right, up, forward, cameraPos, cameraRight,
                                                   cameraUp, cameraFwd, tanX, tanY,
                                                   2.0f * r / static_cast<float>(size));

        static const char* const kScopes[kCascades] = { "c0", "c1", "c2", "c3" };
        static const char* const kSpans[kCascades] = { "sun c0", "sun c1", "sun c2", "sun c3" };
        gpuTimerMark(kSpans[i]);         // [DEV] BF2_GPU_TIMERS
        drawStatsSetScope(kScopes[i]);   // [DEV] BF2_DRAW_STATS: caster draws per cascade
        const unsigned int drawsBefore = frameDrawCalls();
        if (terrain)
            terrain->drawShadowCasters(viewProjection, cull);
        renderQueuesDrawShadowCasters(key, viewProjection, cull);
        g_cascadeDraws[i] = frameDrawCalls() - drawsBefore;
        drawStatsSetScope(0);

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
        constants[4 * 21 + i] = k.coverFar;                     // RASM[21]: coverage ends
        constants[4 * 18 + i] = 2.0f * r / static_cast<float>(size);   // RASM[18]: texel size
        constants[4 * 19 + i] = 1.0f / zr;                      // RASM[19]: depth per metre
    }
    // RASM[17]: toward the sun, and the view depth the blend to baked sunlight starts at.
    constants[4 * 17 + 0] = -forward.x;
    constants[4 * 17 + 1] = -forward.y;
    constants[4 * 17 + 2] = -forward.z;
    constants[4 * 17 + 3] = 0.9f * cascades[kCascades - 1].splitFar;
    constants[4 * 20 + 0] = 1.0f / static_cast<float>(size * 2);   // RASM[20]: atlas texel
    constants[4 * 20 + 1] = g_settings.normalOffset;                 // RASM[20].y, .z: receiver
    constants[4 * 20 + 2] = g_settings.depthBias;                    //   offsets in cascade texels

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
