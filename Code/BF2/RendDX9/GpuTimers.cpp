// [DEV] BF2_GPU_TIMERS=<path>: GPU time per renderer pass. Not part of retail BF2.
//
// Each mark issues a D3D9 timestamp query; the span it starts runs to the next mark, or to the
// timestamp issued after Present. A frame's queries sit in one of kFrames slots and are read
// kFrames - 1 frames later without waiting, so the timers never stall the CPU on the GPU; a frame
// whose results are not ready by then, or whose disjoint query reports a clock change, is dropped.
// Once a second <path> is rewritten (TSV): per span, the mean over the last second and over the
// session, and the last second's maximum, in milliseconds.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "GpuTimers.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;

namespace {

const unsigned int kFrames = 4;

struct Mark
{
    const char* name;
    IDirect3DQuery9* query;
};

struct Frame
{
    IDirect3DQuery9* disjoint;
    IDirect3DQuery9* frequency;
    std::vector<Mark> marks;   // queries are kept across frames; `used` of them are live
    unsigned int used;
    bool pending;              // issued and not yet collected
};

struct Span
{
    std::string name;
    double sessionMs;
    unsigned int sessionFrames;
    double secondMs;
    double secondMax;
    unsigned int secondFrames;
    double frameMs;            // the frame being collected
};

const char* g_path = 0;
Frame* g_frames = 0;
unsigned int g_current = 0;
bool g_open = false;           // the current slot has its disjoint query begun
std::vector<Span>* g_spans = 0;
unsigned int g_collected = 0;
unsigned int g_dropped = 0;
unsigned int g_secondCollected = 0;
std::clock_t g_lastWrite = 0;

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

void releaseQuery(IDirect3DQuery9*& q)
{
    if (q)
        q->Release();
    q = 0;
}

IDirect3DQuery9* createQuery(D3DQUERYTYPE type)
{
    IDirect3DDevice9* const d = device();
    IDirect3DQuery9* q = 0;
    if (!d || d->CreateQuery(type, &q) < 0)
        return 0;
    return q;
}

Span& span(const char* name)
{
    for (size_t i = 0; i < g_spans->size(); ++i)
        if ((*g_spans)[i].name == name)
            return (*g_spans)[i];
    Span s = { name, 0.0, 0, 0.0, 0.0, 0, 0.0 };
    g_spans->push_back(s);
    return g_spans->back();
}

// Reads a finished slot; false while its results are not ready.
bool collect(Frame& f)
{
    BOOL disjoint = TRUE;
    UINT64 frequency = 0;
    if (f.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK
        || f.frequency->GetData(&frequency, sizeof(frequency), 0) != S_OK)
        return false;
    std::vector<UINT64> stamps(f.used);
    for (unsigned int i = 0; i < f.used; ++i)
        if (f.marks[i].query->GetData(&stamps[i], sizeof(UINT64), 0) != S_OK)
            return false;
    if (disjoint || !frequency || f.used < 2)
        return true;   // done, but not usable

    for (size_t i = 0; i < g_spans->size(); ++i)
        (*g_spans)[i].frameMs = -1.0;
    double totalMs = 0.0;
    for (unsigned int i = 0; i + 1 < f.used; ++i)
    {
        const double ms = static_cast<double>(stamps[i + 1] - stamps[i]) * 1000.0
            / static_cast<double>(frequency);
        Span& s = span(f.marks[i].name);
        s.frameMs = (s.frameMs < 0.0 ? 0.0 : s.frameMs) + ms;
        totalMs += ms;
    }
    Span& total = span("(frame)");
    total.frameMs = totalMs;
    for (size_t i = 0; i < g_spans->size(); ++i)
    {
        Span& s = (*g_spans)[i];
        if (s.frameMs < 0.0)
            continue;
        s.sessionMs += s.frameMs;
        ++s.sessionFrames;
        s.secondMs += s.frameMs;
        ++s.secondFrames;
        if (s.frameMs > s.secondMax)
            s.secondMax = s.frameMs;
    }
    ++g_collected;
    ++g_secondCollected;
    return true;
}

void write()
{
    std::string tmp(g_path);
    tmp += ".tmp";
    std::FILE* out = std::fopen(tmp.c_str(), "w");
    if (!out)
        return;
    std::fprintf(out, "#frames\t%u\tdropped\t%u\tlast_second\t%u\n", g_collected, g_dropped,
                 g_secondCollected);
    std::fprintf(out, "#span\tsecond_ms\tsecond_max_ms\tsession_ms\tframes\n");
    for (size_t i = 0; i < g_spans->size(); ++i)
    {
        Span& s = (*g_spans)[i];
        std::fprintf(out, "%s\t%.3f\t%.3f\t%.3f\t%u\n", s.name.c_str(),
                     s.secondFrames ? s.secondMs / s.secondFrames : 0.0, s.secondMax,
                     s.sessionFrames ? s.sessionMs / s.sessionFrames : 0.0, s.sessionFrames);
        s.secondMs = 0.0;
        s.secondMax = 0.0;
        s.secondFrames = 0;
    }
    g_secondCollected = 0;
    std::fclose(out);
    std::remove(g_path);
    std::rename(tmp.c_str(), g_path);
}

bool init()
{
    const char* const v = std::getenv("BF2_GPU_TIMERS");
    if (!v || !*v)
        return false;
    g_path = v;
    g_frames = new Frame[kFrames];
    for (unsigned int i = 0; i < kFrames; ++i)
    {
        g_frames[i].disjoint = 0;
        g_frames[i].frequency = 0;
        g_frames[i].used = 0;
        g_frames[i].pending = false;
    }
    g_spans = new std::vector<Span>();
    return true;
}

// Opens the current slot: collects what it still holds, then begins its disjoint query.
bool open()
{
    if (g_open)
        return true;
    Frame& f = g_frames[g_current];
    if (f.pending)
    {
        if (!collect(f))
            ++g_dropped;   // not ready after kFrames - 1 frames: drop it rather than wait
        f.pending = false;
    }
    if (!f.disjoint)
        f.disjoint = createQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT);
    if (!f.frequency)
        f.frequency = createQuery(D3DQUERYTYPE_TIMESTAMPFREQ);
    if (!f.disjoint || !f.frequency)
        return false;
    f.used = 0;
    f.disjoint->Issue(D3DISSUE_BEGIN);
    g_open = true;
    return true;
}

} // namespace

// The environment is read once, when RendDX9 loads.
bool g_gpuTimersOn = init();

void gpuTimerMark(const char* name)
{
    if (!g_gpuTimersOn || !device() || !open())
        return;
    Frame& f = g_frames[g_current];
    if (f.used == f.marks.size())
    {
        Mark m = { name, createQuery(D3DQUERYTYPE_TIMESTAMP) };
        if (!m.query)
            return;
        f.marks.push_back(m);
    }
    Mark& m = f.marks[f.used++];
    m.name = name;
    m.query->Issue(D3DISSUE_END);
}

void gpuTimersFrame()
{
    if (!g_gpuTimersOn || !g_open)
        return;
    gpuTimerMark("(end)");   // the last span's end; never itself a span
    Frame& f = g_frames[g_current];
    f.frequency->Issue(D3DISSUE_END);
    f.disjoint->Issue(D3DISSUE_END);
    f.pending = true;
    g_open = false;
    g_current = (g_current + 1) % kFrames;

    const std::clock_t now = std::clock();
    if (now - g_lastWrite >= CLOCKS_PER_SEC)
    {
        g_lastWrite = now;
        write();
    }
}

void gpuTimersRelease()
{
    if (!g_gpuTimersOn)
        return;
    for (unsigned int i = 0; i < kFrames; ++i)
    {
        Frame& f = g_frames[i];
        releaseQuery(f.disjoint);
        releaseQuery(f.frequency);
        for (size_t k = 0; k < f.marks.size(); ++k)
            releaseQuery(f.marks[k].query);
        f.marks.clear();
        f.used = 0;
        f.pending = false;
    }
    g_open = false;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
