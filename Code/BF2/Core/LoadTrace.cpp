// [DEV] BF2_LOAD_TRACE writer. See LoadTrace.h. Compiled into BF2.exe and RendDX9.dll; each image
// has its own copy of the state below and so its own output file.

#include "LoadTrace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dice {
namespace hfe {
namespace loadtrace {

namespace {

struct State
{
    std::mutex  lock;
    std::FILE*  file = nullptr;
    char        buffer[1 << 18];
    std::atomic<int> on{-1};       // -1 untested, 0 off, 1 writing

    ~State() { if (file) std::fclose(file); }
};

State& state()
{
    static State s;
    return s;
}

std::string imageName()
{
#if defined(_WIN32)
    HMODULE module = 0;
    char path[MAX_PATH] = {0};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&imageName), &module)
        && GetModuleFileNameA(module, path, MAX_PATH))
    {
        std::string name(path);
        const std::string::size_type slash = name.find_last_of("\\/");
        if (slash != std::string::npos)
            name.erase(0, slash + 1);
        const std::string::size_type dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name.erase(dot);
        for (std::string::size_type i = 0; i < name.size(); ++i)
            if (name[i] >= 'A' && name[i] <= 'Z')
                name[i] = static_cast<char>(name[i] - 'A' + 'a');
        return name;
    }
#endif
    return "main";
}

unsigned threadId()
{
#if defined(_WIN32)
    return static_cast<unsigned>(GetCurrentThreadId());
#else
    return static_cast<unsigned>(std::hash<std::thread::id>()(std::this_thread::get_id()));
#endif
}

// Called with the lock held.
void open(State& s)
{
    s.on = 0;
    const char* env = std::getenv("BF2_LOAD_TRACE");
    if (!env || !*env || (env[0] == '0' && !env[1]))
        return;
    std::string path = (env[0] == '1' && !env[1]) ? std::string("loadtrace") : std::string(env);
    path += "-" + imageName() + ".tsv";
    s.file = std::fopen(path.c_str(), "w");
    if (!s.file)
    {
        std::fprintf(stderr, "[LOADTRACE] cannot write %s\n", path.c_str());
        return;
    }
    std::setvbuf(s.file, s.buffer, _IOFBF, sizeof(s.buffer));
    std::fprintf(s.file, "#tid\tt0_us\tt1_us\tcategory\tbytes\tname\n");
    std::fprintf(stderr, "[LOADTRACE] writing %s\n", path.c_str());
    s.on = 1;
}

void write(const char* category, const char* name, long long t0, long long t1, long long bytes, bool flush)
{
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if (s.on < 0)
        open(s);
    if (s.on != 1)
        return;
    // One formatted row, one fwrite: per-character writes take the stream lock each time, which
    // shows up as self time of the enclosing span when spans are dense (con.cmd).
    char row[640];
    int n = std::snprintf(row, sizeof(row) - 2, "%u\t%lld\t%lld\t%s\t%lld\t", threadId(), t0, t1,
                          category, bytes);
    if (n < 0)
        n = 0;
    if (n > static_cast<int>(sizeof(row)) - 2)
        n = static_cast<int>(sizeof(row)) - 2;
    // Names are paths; keep each row on one line.
    for (const char* c = name ? name : ""; *c && n < static_cast<int>(sizeof(row)) - 1; ++c)
        row[n++] = (*c == '\t' || *c == '\n' || *c == '\r') ? ' ' : *c;
    row[n++] = '\n';
    std::fwrite(row, 1, static_cast<std::size_t>(n), s.file);
    if (flush)
        std::fflush(s.file);
}

} // namespace

bool enabled()
{
    State& s = state();
    if (s.on < 0)
    {
        std::lock_guard<std::mutex> guard(s.lock);
        if (s.on < 0)
            open(s);
    }
    return s.on == 1;
}

long long now()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void span(const char* category, const char* name, long long t0, long long t1, long long bytes)
{
    if (enabled())
        write(category, name, t0, t1, bytes, false);
}

void mark(const char* name)
{
    if (enabled())
    {
        const long long t = now();
        write("mark", name, t, t, 0, true);
    }
}

} // namespace loadtrace
} // namespace hfe
} // namespace dice
