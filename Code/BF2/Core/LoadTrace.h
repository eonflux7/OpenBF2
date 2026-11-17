// [DEV] BF2_LOAD_TRACE: timed spans of load work (files, scripts, textures, meshes, effects, Python).
//
// Set BF2_LOAD_TRACE=<path prefix> (or =1 for the prefix "loadtrace"). Each image that compiles
// LoadTrace.cpp (BF2.exe, RendDX9.dll) writes its own <prefix>-<image>.tsv; the clock is the shared
// steady clock, so the files merge into one timeline. Code/Tools/LoadTrace/loadtrace.py nests the
// spans per thread and reports self time per category, per phase. When the variable is unset every
// call below is a cached flag test and nothing is written.
//
// Row: thread id, start us, end us, category, bytes, name. A mark is a row with start == end and
// category "mark"; it also flushes the file, so a trace survives a later crash up to the last mark.

#ifndef CORE_LOADTRACE_H
#define CORE_LOADTRACE_H

namespace dice {
namespace hfe {
namespace loadtrace {

bool      enabled();
long long now();      // microseconds on the process-wide steady clock
void      span(const char* category, const char* name, long long t0, long long t1, long long bytes = 0);
void      mark(const char* name);

// Records [construction, destruction) as one span. name must outlive the scope. Spans shorter
// than minMicros are dropped (for sites that also run every frame in game).
class Scope
{
public:
    Scope(const char* category, const char* name, long long minMicros = 0)
        : m_category(category), m_name(name), m_t0(enabled() ? now() : -1), m_bytes(0),
          m_minMicros(minMicros) {}
    ~Scope()
    {
        if (m_t0 >= 0)
        {
            const long long t1 = now();
            if (t1 - m_t0 >= m_minMicros)
                span(m_category, m_name, m_t0, t1, m_bytes);
        }
    }
    // The outcome is often known only at the end (archive hit, loose file, miss).
    void setCategory(const char* category) { m_category = category; }
    void addBytes(long long bytes) { m_bytes += bytes; }

private:
    Scope(const Scope&);
    Scope& operator=(const Scope&);

    const char* m_category;
    const char* m_name;
    long long   m_t0;
    long long   m_bytes;
    long long   m_minMicros;
};

} // namespace loadtrace
} // namespace hfe
} // namespace dice

#endif // CORE_LOADTRACE_H
