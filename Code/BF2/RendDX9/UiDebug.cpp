// [UI] The debug menu and the performance overlay (docs/eon64/ui.md, step 3). Not part of retail BF2.
//
// Menu (the menu key): the overlay switch, a button per line of the mod's command file, and the
// renderer's console variables as widgets generated from the console registry (range and help
// from the registry and rendererVariableHelp). A widget sets its variable through the registry,
// as the console does; the menu owns no setting.
//
// Overlay (renderer.uiOverlay), top right: frame rate and a frame-time graph (frame, CPU, GPU),
// GPU time per pass from the timestamp queries (issued only while the overlay is shown), draw
// calls, process memory, display mode, map, camera and sun. Nothing here runs while it is hidden.
//
// Files, in the mod's settings directory (<GSModDirectory>/settings/), never in this repository:
//   eon64-ui.con        console lines run when the game hands the UI its host; rewritten when the
//                       overlay is switched, so the switch is kept between sessions.
//   eon64-commands.txt  one button per line, "label = console line"; '#' starts a comment. Each
//                       button runs its line as if typed into the console (rcon lines included).
#ifdef BF2_IMAGE_RENDDX9

#ifndef NOMINMAX
#define NOMINMAX          // std::min and std::max below
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2   // GetProcessMemoryInfo from kernel32 (K32GetProcessMemoryInfo)
#endif
#include <windows.h>
#include <psapi.h>

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "GpuTimers.h"
#include "UiLayer.h"
#include "UiPanels.h"
#include "../RendMisc/IRendererUi.h"
#include "../RendMisc/IRenderView.h"
#include "../Math/Mat4.h"
#include "../Core/ConsoleObject.h"
#include "../Core/ConsoleObjects.h"
#include "../IO/Console/Console.h"
#include "../Geom/XpLightManager.h"
#include "../Game/Objects/Visual/LightSource.h"

#include "../External/imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace dice {
namespace hfe {

namespace geom { extern LightManager* g_LightManager; }
extern IBase* mainConsole;

namespace dx9rend {

extern void* g_rendererBase;

namespace {

const char kSettingsFile[] = "eon64-ui.con";
const char kCommandsFile[] = "eon64-commands.txt";
const float kPi = 3.14159265f;

RendDX9Base* base()
{
    return static_cast<RendDX9Base*>(g_rendererBase);
}

io::Console* console()
{
    return reinterpret_cast<io::Console*>(mainConsole);
}

// ---------------------------------------------------------------------------------------------
// Settings and commands

IUiHost* g_host = 0;
bool g_overlay = false;
bool g_loadingSettings = false;

struct Command
{
    std::string label;
    std::string line;
};
std::vector<Command> g_commands;

std::string trim(const std::string& s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string settingsPath(const char* file)
{
    std::string dir = g_host ? g_host->uiModDirectory() : "";
    if (dir.empty())
        return std::string();
    if (dir[dir.size() - 1] != '/' && dir[dir.size() - 1] != '\\')
        dir += '/';
    return dir + "settings/" + file;
}

bool readLines(const std::string& path, std::vector<std::string>& lines)
{
    std::FILE* in = path.empty() ? 0 : std::fopen(path.c_str(), "r");
    if (!in)
        return false;
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), in))
        lines.push_back(trim(buffer));
    std::fclose(in);
    return true;
}

void saveSettings()
{
    if (g_loadingSettings || !g_host)
        return;
    const std::string path = settingsPath(kSettingsFile);
    std::FILE* out = path.empty() ? 0 : std::fopen(path.c_str(), "w");
    if (!out)
        return;
    std::fprintf(out, "rem Written by the openbf2-eon64 debug menu.\n");
    std::fprintf(out, "renderer.uiOverlay %d\n", g_overlay ? 1 : 0);
    std::fclose(out);
}

void loadSettings()
{
    std::vector<std::string> lines;
    if (!readLines(settingsPath(kSettingsFile), lines) || !console())
        return;
    g_loadingSettings = true;
    std::string output;   // Console::ignoredString_ lives in BF2.exe and is not exported
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].empty() || lines[i].compare(0, 3, "rem") == 0)
            continue;
        console()->executeLine(lines[i] + "\n", output, false, true, false, false, true);
    }
    g_loadingSettings = false;
}

void loadCommands()
{
    g_commands.clear();
    std::vector<std::string> lines;
    readLines(settingsPath(kCommandsFile), lines);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string& l = lines[i];
        const size_t eq = l.find('=');
        if (l.empty() || l[0] == '#' || eq == std::string::npos)
            continue;
        Command c = { trim(l.substr(0, eq)), trim(l.substr(eq + 1)) };
        if (!c.label.empty() && !c.line.empty())
            g_commands.push_back(c);
    }
    std::fprintf(stderr, "[UI] %u menu commands from %s\n", static_cast<unsigned int>(g_commands.size()),
                 settingsPath(kCommandsFile).c_str());
}

} // namespace

// Runs a line as if typed into the console and entered: the console's own access rules, echo
// and history, and rcon lines go to the server.
void uiRunTyped(const std::string& text)
{
    io::Console* const c = console();
    if (!c)
        return;
    std::string line = text.substr(0, 127);   // the console's line buffer holds 128 with Enter
    c->setCurrentLine(line);
    c->updateAsciiKey(13);
}

namespace {

// ---------------------------------------------------------------------------------------------
// Frame timing

struct Sample
{
    double time;       // seconds
    float frameMs;     // Present to Present
    float cpuMs;       // the frame without the time Present blocked
    float gpuMs;       // the GPU's frame, from the timestamp queries (a few frames old)
};

const unsigned int kSamples = 2048;
const double kWindowSeconds = 5.0;
Sample g_samples[kSamples];
unsigned int g_sampleHead = 0;    // next write
unsigned int g_sampleCount = 0;
double g_ticksPerMs = 0.0;
LONGLONG g_frameStart = 0;
LONGLONG g_presentStart = 0;
double g_presentWaitMs = 0.0;

LONGLONG ticks()
{
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return now.QuadPart;
}

const Sample& sample(unsigned int age)   // 0 = newest
{
    return g_samples[(g_sampleHead + kSamples - 1 - age) % kSamples];
}

struct FrameStats
{
    float avgMs, lowMs, worstMs;     // mean, 99th percentile, worst over the window
    float frameMs, cpuMs, gpuMs;     // smoothed latest
    unsigned int frames;
};

FrameStats frameStats()
{
    FrameStats s = {};
    if (!g_sampleCount)
        return s;
    const double now = sample(0).time;
    std::vector<float> window;
    window.reserve(g_sampleCount);
    double sum = 0.0;
    for (unsigned int i = 0; i < g_sampleCount; ++i)
    {
        const Sample& x = sample(i);
        if (now - x.time > kWindowSeconds)
            break;
        window.push_back(x.frameMs);
        sum += x.frameMs;
        if (x.frameMs > s.worstMs)
            s.worstMs = x.frameMs;
    }
    s.frames = static_cast<unsigned int>(window.size());
    s.avgMs = static_cast<float>(sum / window.size());
    const size_t p99 = (window.size() * 99) / 100;
    std::nth_element(window.begin(), window.begin() + std::min(p99, window.size() - 1), window.end());
    s.lowMs = window[std::min(p99, window.size() - 1)];
    // The latest values, averaged over the last 30 frames so they can be read.
    const unsigned int recent = std::min(g_sampleCount, 30u);
    for (unsigned int i = 0; i < recent; ++i)
    {
        s.frameMs += sample(i).frameMs / recent;
        s.cpuMs += sample(i).cpuMs / recent;
        s.gpuMs += sample(i).gpuMs / recent;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Menu: variables

float uiScale()
{
    return ImGui::GetFontSize() / 16.0f;
}

std::vector<ConsoleObject*> g_variables;   // renderer set/get bindings, sorted by name
size_t g_variablesSeen = 0;                // registry size they were gathered at

bool lessByName(const ConsoleObject* a, const ConsoleObject* b)
{
    return _stricmp(a->getFunctionName(), b->getFunctionName()) < 0;
}

void gatherVariables()
{
    if (!mainConsoleObjects)
        return;
    const ConsoleObjects::ConsoleObjectList& list = mainConsoleObjects->getConsoleObjects();
    if (list.size() == g_variablesSeen)
        return;
    g_variablesSeen = list.size();
    g_variables.clear();
    for (ConsoleObjects::ConsoleObjectList::const_iterator it = list.begin(); it != list.end(); ++it)
    {
        ConsoleObject* const o = *it;
        if (o->isSetGet() && o->getObjectName() && _stricmp(o->getObjectName(), "renderer") == 0)
            g_variables.push_back(o);
    }
    std::sort(g_variables.begin(), g_variables.end(), lessByName);
}

bool isType(const std::string& type, const char* a, const char* b = 0, const char* c = 0)
{
    return type == a || (b && type == b) || (c && type == c);
}

void setVariable(ConsoleObject* o, const std::string& value)
{
    std::string arg(value);
    o->execute(&arg, 1);
}

void variableTooltip(ConsoleObject* o, const std::string& type)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        return;
    ImGui::BeginTooltip();
    const char* const help = rendererVariableHelp(o->getFunctionName());
    if (help)
        ImGui::TextUnformatted(help);
    if (o->hasRange())
    {
        const std::pair<std::string, std::string> r = o->getRangeAsString();
        ImGui::TextDisabled("renderer.%s  %s  %s .. %s", o->getFunctionName(), type.c_str(),
                            r.first.c_str(), r.second.c_str());
    }
    else
        ImGui::TextDisabled("renderer.%s  %s", o->getFunctionName(), type.c_str());
    ImGui::EndTooltip();
}

void drawVariable(ConsoleObject* o)
{
    const char* const name = o->getFunctionName();
    const std::string type = o->getReturnType() ? o->getReturnType() : "";
    ImGui::PushID(o);
    if (!o->isActive())
    {
        ImGui::TextDisabled("%s (inactive)", name);
        variableTooltip(o, type);
        ImGui::PopID();
        return;
    }
    const std::string value = o->execute(0, 0);
    if (isType(type, "bool"))
    {
        bool v = value == "1" || value == "true";
        if (ImGui::Checkbox(name, &v))
            setVariable(o, v ? "1" : "0");
    }
    else if (isType(type, "float", "double") && o->hasRange())
    {
        const std::pair<std::string, std::string> r = o->getRangeAsString();
        float v = static_cast<float>(std::atof(value.c_str()));
        if (ImGui::SliderFloat(name, &v, static_cast<float>(std::atof(r.first.c_str())),
                               static_cast<float>(std::atof(r.second.c_str())), "%.3g"))
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", v);
            setVariable(o, text);
        }
    }
    else if (isType(type, "int", "uint", "unsigned int") && o->hasRange())
    {
        const std::pair<std::string, std::string> r = o->getRangeAsString();
        int v = std::atoi(value.c_str());
        if (ImGui::SliderInt(name, &v, std::atoi(r.first.c_str()), std::atoi(r.second.c_str())))
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%d", v);
            setVariable(o, text);
        }
    }
    else
    {
        // Anything else is edited as text and applied with Enter, as typed into the console.
        char text[256];
        std::snprintf(text, sizeof(text), "%s", value.c_str());
        if (ImGui::InputText(name, text, sizeof(text), ImGuiInputTextFlags_EnterReturnsTrue))
            setVariable(o, text);
    }
    variableTooltip(o, type);
    ImGui::PopID();
}

bool isSunShadowVariable(const ConsoleObject* o)
{
    return std::strncmp(o->getFunctionName(), "sunShadow", 9) == 0;
}

bool containsNoCase(const char* text, const char* part)
{
    if (!*part)
        return true;
    for (; *text; ++text)
        if (_strnicmp(text, part, std::strlen(part)) == 0)
            return true;
    return false;
}

void drawCommands()
{
    if (g_commands.empty())
    {
        ImGui::TextDisabled("No commands: one \"label = console line\" per line in");
        ImGui::TextDisabled("%s", settingsPath(kCommandsFile).c_str());
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i < g_commands.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(g_commands[i].label.c_str()))
            uiRunTyped(g_commands[i].line);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", g_commands[i].line.c_str());
        const float lastRight = ImGui::GetItemRectMax().x;
        if (i + 1 < g_commands.size())
        {
            const float next = lastRight + style.ItemSpacing.x
                + ImGui::CalcTextSize(g_commands[i + 1].label.c_str()).x + style.FramePadding.x * 2.0f;
            if (next < right)
                ImGui::SameLine();
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------------------------
// Overlay

void drawGraph(float width, float height, float worstMs)
{
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 end(origin.x + width, origin.y + height);
    draw->AddRectFilled(origin, end, IM_COL32(0, 0, 0, 110));

    // The scale: the smallest of these that holds the worst frame.
    static const float kScales[] = { 16.7f, 33.3f, 50.0f, 100.0f, 250.0f, 1000.0f };
    float topMs = kScales[0];
    for (unsigned int i = 0; i < sizeof(kScales) / sizeof(kScales[0]) && topMs < worstMs; ++i)
        topMs = kScales[i];
    const float yOf16 = end.y - height * (16.7f / topMs);
    draw->AddLine(ImVec2(origin.x, yOf16), ImVec2(end.x, yOf16), IM_COL32(255, 255, 255, 50));

    if (g_sampleCount < 2)
    {
        ImGui::Dummy(ImVec2(width, height));
        return;
    }
    const double now = sample(0).time;
    std::vector<ImVec2> frame, cpu, gpu;
    for (unsigned int i = 0; i < g_sampleCount; ++i)
    {
        const Sample& s = sample(i);
        const double age = now - s.time;
        if (age > kWindowSeconds)
            break;
        const float x = end.x - static_cast<float>(age / kWindowSeconds) * width;
        const auto y = [&](float ms) { return end.y - height * std::min(ms / topMs, 1.0f); };
        frame.push_back(ImVec2(x, y(s.frameMs)));
        cpu.push_back(ImVec2(x, y(s.cpuMs)));
        if (s.gpuMs > 0.0f)
            gpu.push_back(ImVec2(x, y(s.gpuMs)));
    }
    const float thickness = 1.0f * uiScale();
    draw->AddPolyline(cpu.data(), static_cast<int>(cpu.size()), IM_COL32(90, 200, 255, 200), 0, thickness);
    if (gpu.size() > 1)
        draw->AddPolyline(gpu.data(), static_cast<int>(gpu.size()), IM_COL32(255, 160, 60, 220), 0, thickness);
    draw->AddPolyline(frame.data(), static_cast<int>(frame.size()), IM_COL32(255, 255, 255, 230), 0, thickness);
    char label[32];
    std::snprintf(label, sizeof(label), "%.0f ms", topMs);
    draw->AddText(ImVec2(origin.x + 3.0f, origin.y + 1.0f), IM_COL32(255, 255, 255, 140), label);
    ImGui::Dummy(ImVec2(width, height));
}

void drawPasses()
{
    GpuSpanTime spans[48];
    const unsigned int n = gpuTimersSnapshot(spans, 48);
    if (!n)
    {
        ImGui::TextDisabled("GPU passes: waiting for timestamps");
        return;
    }
    if (!ImGui::BeginTable("passes", 2, ImGuiTableFlags_SizingFixedFit))
        return;
    for (unsigned int i = 0; i < n; ++i)
    {
        if (std::strcmp(spans[i].name, "(frame)") == 0)
            continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(spans[i].name);
        ImGui::TableNextColumn();
        ImGui::Text("%6.2f ms", spans[i].ms);
    }
    ImGui::EndTable();
}

void drawCounts()
{
    const unsigned int* const stats = reinterpret_cast<const unsigned int*>(base()->renderStats);
    // The last frame's row (renderStats words 54..94): indexed, plain and user-pointer draws.
    const unsigned int draws = stats[72] + stats[74] + stats[76];
    ImGui::Text("Draws %u  (%.2fM primitives)", draws, stats[71] / 1.0e6f);
    ImGui::Text("Sun draws %u / %u / %u / %u", sunShadowCascadeDraws(0), sunShadowCascadeDraws(1),
                sunShadowCascadeDraws(2), sunShadowCascadeDraws(3));

    static double lastMemory = -1.0;
    static PROCESS_MEMORY_COUNTERS_EX memory = {};
    const double now = ImGui::GetTime();
    if (now - lastMemory > 0.5 || lastMemory < 0.0)
    {
        lastMemory = now;
        ::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    }
    ImGui::Text("Memory %.0f MB private, %.0f MB working set", memory.PrivateUsage / 1048576.0,
                memory.WorkingSetSize / 1048576.0);
}

void drawWhere()
{
    const D3DPRESENT_PARAMETERS& pp = base()->pp;
    const int samples = pp.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES ? static_cast<int>(pp.MultiSampleType) : 0;
    const char* const map = g_host ? g_host->uiMapName() : "";
    const char* const mode = g_host ? g_host->uiGameMode() : "";
    if (samples)
        ImGui::Text("%ux%u  MSAA %dx", pp.BackBufferWidth, pp.BackBufferHeight, samples);
    else
        ImGui::Text("%ux%u  no MSAA", pp.BackBufferWidth, pp.BackBufferHeight);
    if (map && *map)
        ImGui::Text("%s  %s", map, mode ? mode : "");

    IRenderView* const view = rendererGetRenderView();
    if (view && view->getViewMatrixPtr())
    {
        const float* const c = view->getViewMatrixPtr()->m;
        float heading = std::atan2(c[8], c[10]) * 180.0f / kPi;
        if (heading < 0.0f)
            heading += 360.0f;
        const float pitch = std::asin(std::max(-1.0f, std::min(1.0f, c[9]))) * 180.0f / kPi;
        ImGui::Text("Camera %.1f %.1f %.1f  heading %.0f  pitch %.0f", c[12], c[13], c[14], heading, pitch);
    }

    world::LightSource* const sky = geom::g_LightManager ? geom::g_LightManager->getSkyLight() : 0;
    if (sky)
    {
        const Vec3& d = sky->getDirection();   // the direction the light travels
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (length > 1e-6f)
        {
            const float elevation = std::asin(std::max(-1.0f, std::min(1.0f, -d.y / length))) * 180.0f / kPi;
            float azimuth = std::atan2(-d.x, -d.z) * 180.0f / kPi;
            if (azimuth < 0.0f)
                azimuth += 360.0f;
            ImGui::Text("Sun elevation %.0f  azimuth %.0f", elevation, azimuth);
        }
    }
}

} // namespace

void uiDebugSetHost(IUiHost* host)
{
    if (g_host || !host)
        return;
    g_host = host;
    loadSettings();
    loadCommands();
}

IUiHost* uiDebugHost()
{
    return g_host;
}

bool uiOverlayShown()
{
    return g_overlay;
}

bool uiLayerGetOverlay()
{
    return g_overlay;
}

void uiLayerSetOverlay(bool on)
{
    if (on == g_overlay)
        return;
    g_overlay = on;
    gpuTimersSetLive(on);
    g_sampleCount = 0;
    g_frameStart = 0;
    saveSettings();
}

void uiDebugFrameStart()
{
    if (!g_overlay)
        return;
    if (g_ticksPerMs == 0.0)
    {
        LARGE_INTEGER frequency;
        ::QueryPerformanceFrequency(&frequency);
        g_ticksPerMs = static_cast<double>(frequency.QuadPart) / 1000.0;
    }
    const LONGLONG now = ticks();
    if (g_frameStart)
    {
        Sample& s = g_samples[g_sampleHead];
        s.time = static_cast<double>(now) / (g_ticksPerMs * 1000.0);
        s.frameMs = static_cast<float>((now - g_frameStart) / g_ticksPerMs);
        s.cpuMs = std::max(0.0f, s.frameMs - static_cast<float>(g_presentWaitMs));
        s.gpuMs = gpuTimersFrameMs();
        g_sampleHead = (g_sampleHead + 1) % kSamples;
        if (g_sampleCount < kSamples)
            ++g_sampleCount;
    }
    g_frameStart = now;
}

void uiDebugBeforePresent()
{
    if (g_overlay)
        g_presentStart = ticks();
}

void uiDebugPresented()
{
    if (g_overlay && g_presentStart)
        g_presentWaitMs = (ticks() - g_presentStart) / g_ticksPerMs;
}

void uiDebugDrawMenu()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * uiScale(), viewport->WorkSize.y * 0.75f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("openbf2-eon64"))
    {
        ImGui::End();
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("%.1f fps  %.2f ms", io.Framerate, io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    bool overlay = g_overlay;
    if (ImGui::Checkbox("Performance overlay", &overlay))
        uiLayerSetOverlay(overlay);

    if (ImGui::CollapsingHeader("Commands", ImGuiTreeNodeFlags_DefaultOpen))
        drawCommands();

    gatherVariables();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    if (ImGui::CollapsingHeader("Sun shadows", ImGuiTreeNodeFlags_DefaultOpen))
        for (size_t i = 0; i < g_variables.size(); ++i)
            if (isSunShadowVariable(g_variables[i]))
                drawVariable(g_variables[i]);
    if (ImGui::CollapsingHeader("Renderer"))
    {
        static char filter[64] = "";
        ImGui::InputTextWithHint("##filter", "filter", filter, sizeof(filter));
        for (size_t i = 0; i < g_variables.size(); ++i)
            if (!isSunShadowVariable(g_variables[i])
                && containsNoCase(g_variables[i]->getFunctionName(), filter))
                drawVariable(g_variables[i]);
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

void uiDebugDrawOverlay()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 8.0f * uiScale();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("##overlay", 0, flags))
    {
        ImGui::End();
        return;
    }
    const FrameStats s = frameStats();
    const auto fps = [](float ms) { return ms > 0.0f ? 1000.0f / ms : 0.0f; };
    ImGui::Text("%.0f fps  %.2f ms", fps(s.frameMs), s.frameMs);
    ImGui::Text("avg %.0f  1%% low %.0f  worst %.1f ms", fps(s.avgMs), fps(s.lowMs), s.worstMs);
    ImGui::TextColored(ImVec4(0.35f, 0.78f, 1.0f, 1.0f), "CPU %.2f ms", s.cpuMs);
    ImGui::SameLine();
    if (s.gpuMs > 0.0f)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.63f, 0.24f, 1.0f), "GPU %.2f ms", s.gpuMs);
        ImGui::SameLine();
        ImGui::TextDisabled(s.gpuMs > s.cpuMs ? "GPU-bound" : "CPU-bound");
    }
    else
        ImGui::TextDisabled("GPU -");
    drawGraph(300.0f * uiScale(), 70.0f * uiScale(), s.worstMs);
    ImGui::Separator();
    drawPasses();
    ImGui::Separator();
    drawCounts();
    ImGui::Separator();
    drawWhere();
    ImGui::End();
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
