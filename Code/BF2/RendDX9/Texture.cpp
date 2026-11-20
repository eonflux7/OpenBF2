// D3D texture ownership, pool-specific locking, staging surfaces, and mip accounting.

#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/BfMemory.h"
#include "../../RendDX9Abi/Frontier.h"
#include "RendDX9Base.h"
#include "Texture.h"
#include "ResourceTracker.h"
#include "../RendMisc/IRenderer.h"
#include "../GeomMisc/Debug.h"
#if defined(_WIN32)
#include <dxerr9.h>
#pragma comment(lib, "dxerr9.lib")
#endif

#include <new>
#include <cstdio>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;   // dword_1023B970 - RendDX9Init.cpp. The ctor/dtor count through it.

// TextureManager.cpp - the four reads the constructor makes on dword_1023B974.
int                 textureManagerDefaultMipClass();   // sub_10017DF0 -> TM+0x98
int                 textureManagerManagedFlag();       // sub_10017E30 -> TM+0x20
unsigned char       textureManagerRecreateMode();      // sub_10017E50 -> TM+0x24
const std::string& textureManagerOriginLabel();       // TM+0x9C

namespace {


// Shared getRef/no-op/addRef table. Owners explicitly decrement, destroy, and free;
// this interface has no release slot.


int  RD9_THUNK tex_getRef(void* self RD9_THUNK_EDX)  { return static_cast<EngineTexture*>(self)->refCount; }
void RD9_THUNK tex_nullsub(void* RD9_THUNK_EDX)      { /* nullsub - empty in the original too */ }
void RD9_THUNK tex_addRef(void* self RD9_THUNK_EDX)  { ++static_cast<EngineTexture*>(self)->refCount; }

void* const kTextureVtable[3] =
{
    reinterpret_cast<void*>(&tex_getRef),     // +0x00
    reinterpret_cast<void*>(&tex_nullsub),    // +0x04
    reinterpret_cast<void*>(&tex_addRef)      // +0x08
};

// The four QI target GUIDs live in Texture.h (byte-read from unk_10208560/70/80/90).

IDirect3DDevice9* baseDevice()
{
    RendDX9Base* b = reinterpret_cast<RendDX9Base*>(g_rendererBase);
    return b ? b->device : 0;
}


// Adjusts the renderer's live-texture statistic.

// PE 0x1000D8D0
void decrementTextureCount(void* stats)
{
    --*reinterpret_cast<int*>(static_cast<unsigned char*>(stats) + 0x2C);
}

void bumpTextureCount(int delta)
{
    // [CHOSEN, not recovered] Retain the existing missing-renderer null-access omission.
    // PE 0x10014B81 / 0x10014E4F dispatch the statistics getter without a guard.
    if (!g_rendererBase)
        return;
    void* const stats = static_cast<IRenderer*>(g_rendererBase)->getRenderStats();
    if (delta == -1)
        decrementTextureCount(stats);
    else
        *reinterpret_cast<int*>(static_cast<unsigned char*>(stats) + 0x2C) += delta;
}

} // anonymous namespace


// sub_10014AF0 - the constructor. `storage` comes from the engine allocator and is NOT zeroed; the
// shipped ctor writes exactly the fields below and leaves the created-desc block (+0x28..+0x43) and
// the bytes at +0x52/+0x53 and +0x79..+0x7B as they land. Reproduced field-for-field, in the
// shipped order where an observer could tell (the string copy and the counter bump come last).
// The placement default-initialization constructs only the name string (+0x58, empty).
// PE 0x10014AF0 (listing read).
EngineTexture* textureCtor(void* storage)
{
    EngineTexture* t = new (storage) EngineTexture;

    t->vptr        = const_cast<void**>(kTextureVtable);
    t->refCount    = 1;
    t->baseTexture = 0;
    t->lockCount   = 0;                                    // +0x44
    t->byte51      = 0;                                    // +0x51
    t->priority    = 0;                                    // +0x54
    t->totalBytes  = 0;                                    // +0x74
    t->usedSuffixedFile = 0;                               // +0x78

    for (int i = 0; i < 7; ++i)                            // +0x0C..+0x27, this[3]..this[9]
        t->fileInfo[i] = 0;

    // deliberately NOT written: width..face (+0x28..+0x43), pad52, pad79 - the shipped ctor
    // skips them and textureAttach qmemcpy's the desc in later.

    t->mipClassIndex      = textureManagerDefaultMipClass();
    t->pool               = textureManagerManagedFlag();
    t->recreateFromStored = textureManagerRecreateMode();

    t->name = textureManagerOriginLabel();                 // string copy from TM+0x9C

    // (*(base->vptr + 0x6C))(base) -> +0x108; PE 0x1000D8C0 -> ++(+0x2C).
    bumpTextureCount(+1);
    return t;
}


// sub_10014E20 - the destructor. Re-stores the vptr first (the shipped compiler's dtor prologue),
// releases the held D3D texture, unregisters it from the resource tracker, decrements the base
// stats counter and destroys the name.

void textureDtor(EngineTexture* tex)
{
    tex->vptr = const_cast<void**>(kTextureVtable);
    if (tex->baseTexture)
    {
        tex->baseTexture->Release();
        resourceTrackerRemove(0, tex->baseTexture);         // PE 0x10014E3B: after Release
        tex->baseTexture = 0;
    }
    bumpTextureCount(-1);                                  // base vslot +0x6C, then sub_1000D8D0
    tex->~EngineTexture();                                 // destroys the name
}

void textureAddRef(EngineTexture* tex)
{
    ++tex->refCount;                                       // vslot +0x08, ICF_incrementDwordAt0x4
}


// sub_10014E70 - release the D3D resource without destroying the wrapper. The default-pool
// device-lost path: Meme::Texture's DeviceLostResetListener slot +0x00 tail-jumps here for a
// pool-0 texture, and onDeviceReset later re-creates through the recreate arm. Returns the D3D
// Release() count; the tracker erase fires only when that hit zero.

int textureReleaseD3DResource(EngineTexture* tex)
{
    if (!tex->baseTexture)
        return 0;
    const int remaining = static_cast<int>(tex->baseTexture->Release());
    if (!remaining)
    {
        resourceTrackerRemove(0, tex->baseTexture);         // PE 0x10014E8C
    }
    tex->baseTexture = 0;
    return remaining;
}

void textureReleaseRef(EngineTexture*& tex)
{
    // The owner-side pattern Font::Ported::load ships: `if (v && !--v[1]) { sub_10014E20; free }`.
    if (tex && !--tex->refCount)
    {
        textureDtor(tex);
        ::renddx9abi::engineFree(tex);
    }
    tex = 0;
}


// Replaces and AddRefs the D3D texture, copies its description, and resets lock state.
// Updates origin name, mip byte count, and resource tracking after releasing the old texture.

// PE 0x10014840 (listing read).
bool textureAttach(EngineTexture* tex, IDirect3DBaseTexture9* d3d, const unsigned int desc[7])
{
    if (tex->baseTexture)
    {
        resourceTrackerRemove(0, tex->baseTexture);         // PE 0x10014854: before Release
        tex->baseTexture->Release();
        tex->baseTexture = 0;
        tex->lockCount   = 0;
    }

    tex->baseTexture = d3d;
    d3d->AddRef();

    ::memcpy(&tex->width, desc, 0x1C);

    tex->name = textureManagerOriginLabel();               // TM+0x9C, again

    int w = static_cast<int>(tex->width), h = static_cast<int>(tex->height),
        d = static_cast<int>(tex->depth);
    const int bpp = bitsPerPixel(tex->format);
    tex->totalBytes = 0;
    for (unsigned int i = 0; i < tex->mipLevels; ++i)
    {
        tex->totalBytes += static_cast<unsigned int>(h * w * d * bpp / 8);
        h >>= 1;
        w >>= 1;
        if (d > 1)
            d >>= 1;
    }

    // PE 0x10014909; mip arithmetic above uses the image's signed divides and shifts.
    resourceTrackerRegister(0, tex->baseTexture, tex->totalBytes, tex->name, &tex->pool, tex);
    return true;
}


// sub_10014820 - qmemcpy(this + 0x0C, info, 0x1C). The original D3DXIMAGE_INFO, stamped at the very
// end of loadTextureFromFile - after the mip-skip has already shrunk the created desc at +0x28, so
// the two blocks deliberately differ on a skipped-mip load.
// PE 0x10014820.
void textureSetFileInfo(EngineTexture* tex, const void* info28)
{
    ::memcpy(tex->fileInfo, info28, 0x1C);
}


// sub_10014950 - the real level surface. face == QI IDirect3DTexture9 and
// GetSurfaceLevel(mip); otherwise QI IDirect3DCubeTexture9 and GetCubeMapSurface(face, mip). The
// QI'd interface is Released before returning; the surface ref passes to the caller.

// PE 0x10014950
IDirect3DSurface9* textureGetLevelSurface(EngineTexture* tex, unsigned int mip, unsigned int face)
{
    IDirect3DSurface9* surf = 0;
    void* itf = 0;
    if (face == 0x7FFFFFFF)
    {
        tex->baseTexture->QueryInterface(kIID_Texture9, &itf);
        static_cast<IDirect3DTexture9*>(itf)->GetSurfaceLevel(mip, &surf);
    }
    else
    {
        tex->baseTexture->QueryInterface(kIID_CubeTexture9, &itf);
        static_cast<IDirect3DCubeTexture9*>(itf)->GetCubeMapSurface(
            static_cast<D3DCUBEMAP_FACES>(face), mip, &surf);
    }
    static_cast<IUnknown*>(itf)->Release();
    return surf;
}

namespace {


// sub_100149E0 - the staging surface for a default-pool lock. Read from the body and its prologue:
// it ignores the level surface its caller passes, reads the device from dword_1023B970+0x0C, and
// creates a fresh 1-level systemmem texture of the mip's exact w/h/format, returning its level-0
// surface (the texture itself is Released; the surface ref survives it).

// PE 0x100149E0. The ignored incoming surface is omitted from this private native adapter.
IDirect3DSurface9* createStagingSurface(unsigned int w, unsigned int h, unsigned int format)
{
    IDirect3DDevice9* device = baseDevice();
    IDirect3DTexture9* staging = 0;
    device->CreateTexture(w, h, 1, 0, static_cast<D3DFORMAT>(format),
                          D3DPOOL_SYSTEMMEM, &staging, 0);
    IDirect3DSurface9* surf = 0;
    staging->GetSurfaceLevel(0, &surf);
    staging->Release();
    return surf;
}


const char* updateErrorDescription(HRESULT result)
{
#if defined(_WIN32)
    return ::DXGetErrorDescription9A(result);
#else
    // [CHOSEN, not recovered] DXErr9 is a Windows SDK library; retain the native HRESULT.
    static char description[32];
    std::snprintf(description, sizeof(description), "HRESULT 0x%08X",
                  static_cast<unsigned int>(result));
    return description;
#endif
}

// PE 0x10014A40
void updateSurfaceFromStaging(IDirect3DSurface9* dst, IDirect3DSurface9* src)
{
    IDirect3DDevice9* device = baseDevice();
    const HRESULT hr = device->UpdateSurface(src, 0, dst, 0);
    if (hr != 0)
    {
        ::renddx9abi::Debug diagnostic(DebugTypeError,
            "C:\\dice\\Projects\\BF2Branches\\Patch_1_50\\Code\\BF2\\RendDX9\\Texture.cpp", 484, "");
        const char* const description = updateErrorDescription(hr);
        diagnostic << "Failed to update texture. " << description;
    }
}

} // anonymous namespace

// PE 0x100149D0. [INFERRED] name: the sole caller passes the loaded texture in ECX,
// pushes no arguments, and discards the result; the entire shipped body is RET.
void textureFinalizeFromCode(EngineTexture*)
{
}


// Locks a mip/face through a staging surface for default-pool textures or directly otherwise.
// Returns a lock record retained until unlock; the temporary queried interface is released.

TextureLockRecord* textureLock(EngineTexture* tex, unsigned int mip, unsigned int face)
{
    TextureLockRecord* rec =
        static_cast<TextureLockRecord*>(::renddx9abi::engineAlloc(sizeof(TextureLockRecord)));
    if (rec)
        ::memset(rec, 0, sizeof(TextureLockRecord));

    rec->width  = tex->width >> mip;
    rec->height = tex->height >> mip;
    unsigned int d = tex->depth >> mip;
    rec->depth  = (static_cast<int>(d) < 1) ? 1 : d;
    rec->bpp    = bitsPerPixel(tex->format);
    rec->mip    = mip;
    rec->face   = face;

    if (tex->pool == 0)
    {
        // default pool - can't lock the real surface; stage in systemmem.
        IDirect3DSurface9* real  = textureGetLevelSurface(tex, mip, face);
        IDirect3DSurface9* stage = createStagingSurface(rec->width, rec->height, tex->format);
        rec->staging = stage;

        D3DLOCKED_RECT lr;
        stage->LockRect(&lr, 0, 0);                        // surface vslot +0x34 (slot 13)
        real->Release();
        rec->pBits = lr.pBits;

        ++tex->lockCount;
        return rec;
    }

    void* itf = 0;
    if (face != 0x7FFFFFFF)
    {
        tex->baseTexture->QueryInterface(kIID_CubeTexture9, &itf);
        D3DLOCKED_RECT lr;
        static_cast<IDirect3DCubeTexture9*>(itf)->LockRect(
            static_cast<D3DCUBEMAP_FACES>(face), mip, &lr, 0, 0);
        static_cast<IUnknown*>(itf)->Release();
        rec->pBits = lr.pBits;
    }
    else if (tex->depth <= 1)
    {
        tex->baseTexture->QueryInterface(kIID_Texture9, &itf);
        D3DLOCKED_RECT lr;
        static_cast<IDirect3DTexture9*>(itf)->LockRect(mip, &lr, 0, 0);
        static_cast<IUnknown*>(itf)->Release();
        rec->pBits = lr.pBits;
    }
    else
    {
        tex->baseTexture->QueryInterface(kIID_VolumeTexture9, &itf);
        D3DLOCKED_BOX lb;
        static_cast<IDirect3DVolumeTexture9*>(itf)->LockBox(mip, &lb, 0, 0);
        static_cast<IUnknown*>(itf)->Release();
        rec->pBits = lb.pBits;
    }

    rec->staging = 0;
    ++tex->lockCount;
    return rec;
}


// sub_10014D30 - unlock. Managed: QI the matching interface and UnlockRect/UnlockBox. Default:
// unlock the staging surface, fetch the real level surface again, UpdateSurface staging -> real,
// release both. Then free the record and --lockCount. Always returns 1.
// PE 0x10014D30.
bool textureUnlock(EngineTexture* tex, TextureLockRecord* rec)
{
    if (tex->pool != 0)
    {
        void* itf = 0;
        if (rec->face == 0x7FFFFFFF)
        {
            if (tex->depth > 1)
            {
                tex->baseTexture->QueryInterface(kIID_VolumeTexture9, &itf);
                static_cast<IDirect3DVolumeTexture9*>(itf)->UnlockBox(rec->mip);
            }
            else
            {
                tex->baseTexture->QueryInterface(kIID_Texture9, &itf);
                static_cast<IDirect3DTexture9*>(itf)->UnlockRect(rec->mip);
            }
        }
        else
        {
            tex->baseTexture->QueryInterface(kIID_CubeTexture9, &itf);
            static_cast<IDirect3DCubeTexture9*>(itf)->UnlockRect(
                static_cast<D3DCUBEMAP_FACES>(rec->face), rec->mip);
        }
        static_cast<IUnknown*>(itf)->Release();
    }
    else
    {
        rec->staging->UnlockRect();                        // surface vslot +0x38 (slot 14)
        IDirect3DSurface9* real = textureGetLevelSurface(tex, rec->mip, rec->face);
        updateSurfaceFromStaging(real, rec->staging);      // Texture_updateSurface, 0x10014A40
        rec->staging->Release();
        real->Release();
    }

    ::renddx9abi::engineFree(rec);
    --tex->lockCount;
    return true;
}

// ---- RGBA8 readback (render milestone 11: the atmosphere fit; tools.survey) ----

namespace {

void rgb565(unsigned int c, unsigned char out[3])
{
    out[0] = static_cast<unsigned char>(((c >> 11) & 31) * 255 / 31);
    out[1] = static_cast<unsigned char>(((c >> 5) & 63) * 255 / 63);
    out[2] = static_cast<unsigned char>((c & 31) * 255 / 31);
}

// One 4x4 DXT block into 16 RGBA texels.
void decodeBlock(const unsigned char* block, D3DFORMAT f, unsigned char out[16][4])
{
    const unsigned char* colour = (f == D3DFMT_DXT1) ? block : block + 8;
    const unsigned int c0 = colour[0] | (colour[1] << 8), c1 = colour[2] | (colour[3] << 8);
    unsigned char p[4][4];
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    p[0][3] = p[1][3] = 255;
    if (f != D3DFMT_DXT1 || c0 > c1)
        for (int k = 0; k < 3; ++k)
        {
            p[2][k] = static_cast<unsigned char>((2 * p[0][k] + p[1][k]) / 3);
            p[3][k] = static_cast<unsigned char>((p[0][k] + 2 * p[1][k]) / 3);
        }
    else
        for (int k = 0; k < 3; ++k)
        {
            p[2][k] = static_cast<unsigned char>((p[0][k] + p[1][k]) / 2);
            p[3][k] = 0;
        }
    p[2][3] = 255;
    p[3][3] = (f == D3DFMT_DXT1 && c0 <= c1) ? 0 : 255;
    const unsigned int bits = colour[4] | (colour[5] << 8) | (colour[6] << 16)
                            | (static_cast<unsigned int>(colour[7]) << 24);
    for (int i = 0; i < 16; ++i)
    {
        const unsigned char* const c = p[(bits >> (2 * i)) & 3];
        out[i][0] = c[0]; out[i][1] = c[1]; out[i][2] = c[2]; out[i][3] = c[3];
    }
    if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3)
        for (int i = 0; i < 16; ++i)
            out[i][3] = static_cast<unsigned char>(((block[i / 2] >> ((i & 1) * 4)) & 15) * 17);
    else if (f == D3DFMT_DXT4 || f == D3DFMT_DXT5)
    {
        unsigned int a[8];
        a[0] = block[0];
        a[1] = block[1];
        if (a[0] > a[1])
            for (int i = 1; i < 7; ++i)
                a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
        else
        {
            for (int i = 1; i < 5; ++i)
                a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
            a[6] = 0;
            a[7] = 255;
        }
        unsigned long long abits = 0;
        for (int i = 0; i < 6; ++i)
            abits |= static_cast<unsigned long long>(block[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i)
            out[i][3] = static_cast<unsigned char>(a[(abits >> (3 * i)) & 7]);
    }
}

} // anonymous namespace

// A mip level as RGBA8. False (with why) for default-pool textures and formats not decoded here.
bool textureReadLevelRgba8(const EngineTexture* t, unsigned int face, unsigned int level,
               std::vector<unsigned char>& rgba, unsigned int& w, unsigned int& h, std::string& why)
{
    if (!t || !t->baseTexture)
    {
        why = "no texture";
        return false;
    }
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    IDirect3DTexture9* tex2d = 0;
    IDirect3DCubeTexture9* cube = 0;
    if (t->resourceType == 3)
    {
        tex2d = static_cast<IDirect3DTexture9*>(t->baseTexture);
        if (level >= tex2d->GetLevelCount() || FAILED(tex2d->GetLevelDesc(level, &desc)))
        {
            why = "no level";
            return false;
        }
    }
    else if (t->resourceType == 5)
    {
        cube = static_cast<IDirect3DCubeTexture9*>(t->baseTexture);
        if (level >= cube->GetLevelCount() || FAILED(cube->GetLevelDesc(level, &desc)))
        {
            why = "no level";
            return false;
        }
    }
    else
    {
        why = "not 2D or cube";
        return false;
    }
    if (desc.Pool == D3DPOOL_DEFAULT)
    {
        why = "default pool";
        return false;
    }
    const D3DFORMAT f = desc.Format;
    const bool dxt = f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4
                  || f == D3DFMT_DXT5;
    if (!dxt && f != D3DFMT_A8R8G8B8 && f != D3DFMT_X8R8G8B8)
    {
        char text[48];
        std::snprintf(text, sizeof(text), "format %u not read", static_cast<unsigned int>(f));
        why = text;
        return false;
    }
    hr = tex2d ? tex2d->LockRect(level, &lr, 0, D3DLOCK_READONLY)
               : cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &lr, 0, D3DLOCK_READONLY);
    if (FAILED(hr))
    {
        char text[32];
        std::snprintf(text, sizeof(text), "lock failed 0x%08x", static_cast<unsigned int>(hr));
        why = text;
        return false;
    }
    w = desc.Width;
    h = desc.Height;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    const unsigned char* const bits = static_cast<const unsigned char*>(lr.pBits);
    if (dxt)
    {
        const unsigned int blockBytes = f == D3DFMT_DXT1 ? 8 : 16;
        for (unsigned int by = 0; by < (h + 3) / 4; ++by)
            for (unsigned int bx = 0; bx < (w + 3) / 4; ++bx)
            {
                unsigned char texels[16][4];
                decodeBlock(bits + by * lr.Pitch + bx * blockBytes, f, texels);
                for (unsigned int i = 0; i < 16; ++i)
                {
                    const unsigned int x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                    if (x < w && y < h)
                        for (int k = 0; k < 4; ++k)
                            rgba[(static_cast<size_t>(y) * w + x) * 4 + k] = texels[i][k];
                }
            }
    }
    else
        for (unsigned int y = 0; y < h; ++y)
            for (unsigned int x = 0; x < w; ++x)
            {
                const unsigned char* const s = bits + y * lr.Pitch + x * 4;   // B G R A
                unsigned char* const d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
                d[3] = f == D3DFMT_X8R8G8B8 ? 255 : s[3];
            }
    if (tex2d)
        tex2d->UnlockRect(level);
    else
        cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), level);
    return true;
}

// The first level no wider than maxWidth (the last level if none is).
unsigned int textureLevelAtMost(const EngineTexture* t, unsigned int maxWidth)
{
    unsigned int level = 0, w = t->width;
    while (w > maxWidth && level + 1 < t->mipLevels)
    {
        w = w > 1 ? w / 2 : 1;
        ++level;
    }
    return level;
}

bool textureMeanColour(const EngineTexture* t, unsigned int face, unsigned int maxWidth, double out[4],
                std::string& why)
{
    std::vector<unsigned char> rgba;
    unsigned int w = 0, h = 0;
    if (!textureReadLevelRgba8(t, face, textureLevelAtMost(t, maxWidth), rgba, w, h, why))
        return false;
    double sum[4] = {};
    for (size_t i = 0; i < rgba.size(); i += 4)
        for (int k = 0; k < 4; ++k)
            sum[k] += rgba[i + k];
    const double n = static_cast<double>(w) * h;
    for (int k = 0; k < 4; ++k)
        out[k] = n > 0 ? sum[k] / n : 0;
    return true;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
