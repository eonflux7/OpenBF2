// [DEV] BF2_DRAW_STATS=<path>: draw counts per effect, technique and pass. Not part of retail BF2.
//
// Shows which effects, techniques and passes actually draw, as opposed to which load. Each Effect
// or Ra shader pass that begins becomes the current entry; the renderer's draw wrappers and
// RaShader::draw add to it. Draws outside any pass count against "(none)". Once a second the table
// is rewritten to <path>: the session totals (does it draw on this map at all) and the last
// presented frame (what this view draws). A scope (drawStatsSetScope) splits a technique's rows by
// what the renderer is drawing it for; counter rows (effect "(count)") record events, not draws.
#ifdef BF2_IMAGE_RENDDX9

#include "DrawStats.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

namespace {

struct Entry
{
    unsigned long long draws;        // session
    unsigned long long primitives;   // session
    unsigned int frames;             // presented frames with at least one draw
    unsigned int frameDraws;         // the frame being drawn
    unsigned int framePrimitives;
    unsigned int lastDraws;          // the last presented frame
    unsigned int lastPrimitives;
};

struct Key
{
    std::string effect;
    std::string technique;
    unsigned int pass;
    bool operator<(const Key& o) const
    {
        if (effect != o.effect) return effect < o.effect;
        if (technique != o.technique) return technique < o.technique;
        return pass < o.pass;
    }
};

typedef std::map<Key, Entry> Table;

const char* g_path = 0;
Table* g_table = 0;
Entry* g_current = 0;
Entry* g_none = 0;
std::vector<Entry*>* g_touched = 0;   // entries drawn in the frame being drawn
std::vector<Entry*>* g_last = 0;      // entries drawn in the last presented frame
unsigned int g_frames = 0;
std::clock_t g_lastWrite = 0;
const char* g_scope = 0;

void add(Entry* e, unsigned int draws, unsigned int primitives)
{
    if (!e->frameDraws)
        g_touched->push_back(e);
    e->draws += draws;
    e->primitives += primitives;
    e->frameDraws += draws;
    e->framePrimitives += primitives;
}

Entry* lookup(const std::string& effect, const std::string& technique, unsigned int pass)
{
    Key key;
    key.effect = effect;
    key.technique = technique;
    key.pass = pass;
    Table::iterator it = g_table->find(key);
    if (it == g_table->end())
    {
        Entry e = {};
        it = g_table->insert(Table::value_type(key, e)).first;
    }
    return &it->second;
}

void write()
{
    std::string tmp(g_path);
    tmp += ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f)
        return;
    std::fprintf(f, "#frames\t%u\n", g_frames);
    std::fprintf(f, "#effect\ttechnique\tpass\tdraws\tprimitives\tframes\tlast_draws\tlast_primitives\n");
    for (Table::const_iterator it = g_table->begin(); it != g_table->end(); ++it)
    {
        const Entry& e = it->second;
        if (!e.draws)
            continue;
        std::fprintf(f, "%s\t%s\t%u\t%llu\t%llu\t%u\t%u\t%u\n", it->first.effect.c_str(),
                     it->first.technique.c_str(), it->first.pass, e.draws, e.primitives, e.frames,
                     e.lastDraws, e.lastPrimitives);
    }
    std::fclose(f);
    std::remove(g_path);
    std::rename(tmp.c_str(), g_path);
}

bool init()
{
    const char* const v = std::getenv("BF2_DRAW_STATS");
    if (!v || !*v)
        return false;
    g_path = v;
    g_table = new Table();
    g_touched = new std::vector<Entry*>();
    g_last = new std::vector<Entry*>();
    g_none = lookup("(none)", "", 0);
    return true;
}

} // namespace

// The environment is read once, when RendDX9 loads.
bool g_drawStatsOn = init();

void drawStatsBeginPass(const std::string& effect, const std::string& technique, unsigned int pass)
{
    if (!g_drawStatsOn)
        return;
    g_current = g_scope ? lookup(effect, technique + "@" + g_scope, pass)
                        : lookup(effect, technique, pass);
}

void drawStatsSetScope(const char* scope)
{
    g_scope = scope;
}

void drawStatsCount(const char* name, unsigned int n)
{
    if (!g_drawStatsOn || !n)
        return;
    add(lookup("(count)", name, 0), n, 0);
}

void drawStatsEndPass()
{
    g_current = 0;
}

void drawStatsDraw(unsigned int primitives)
{
    if (!g_drawStatsOn)
        return;
    add(g_current ? g_current : g_none, 1, primitives);
}

void drawStatsFrame()
{
    if (!g_drawStatsOn)
        return;
    ++g_frames;
    for (size_t i = 0; i < g_last->size(); ++i)
    {
        (*g_last)[i]->lastDraws = 0;
        (*g_last)[i]->lastPrimitives = 0;
    }
    for (size_t i = 0; i < g_touched->size(); ++i)
    {
        Entry* const e = (*g_touched)[i];
        ++e->frames;
        e->lastDraws = e->frameDraws;
        e->lastPrimitives = e->framePrimitives;
        e->frameDraws = 0;
        e->framePrimitives = 0;
    }
    g_last->swap(*g_touched);
    g_touched->clear();

    const std::clock_t now = std::clock();
    if (now - g_lastWrite >= CLOCKS_PER_SEC)
    {
        g_lastWrite = now;
        write();
    }
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
