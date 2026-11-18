// [UI] The debug menu and the performance overlay (docs/eon64/ui.md, step 3). Not part of retail BF2.
//
// Menu (the menu key), in tabs:
//   Commands  the mod's command file as buttons, grouped by section, with fields for arguments.
//   Shadows   the sun shadow variables.   Renderer  every other renderer.* variable, filtered.
//   Settings  the overlay, the console height, the menu key, the files.
// Variable widgets are generated from the console registry (range from the registry, help from
// rendererVariableHelp) and set their variable through the registry, as the console does; the menu
// owns no setting. Reset restores the value a variable had when the menu first showed it.
//
// Overlay (renderer.uiOverlay), top right: frame rate and a frame-time graph (frame, CPU, GPU),
// GPU time per pass from the timestamp queries (issued only while the overlay is shown), draw
// calls, process memory, display mode, map, camera and sun. Nothing here runs while it is hidden.
//
// Files, in the mod's settings directory (<GSModDirectory>/settings/), never in this repository:
//   eon64-ui.con        console lines run when the game hands the UI its host; rewritten when a UI
//                       setting changes (overlay, console height, menu key).
//   eon64-commands.txt  "[Section]" lines group; "label = console line | help" makes a button;
//                       "{name}" or "{name=default}" in the line adds a field; '#' starts a
//                       comment. A button runs its line as if typed into the console (rcon too).
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
#include <cctype>
#include <cstring>
#include <map>
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

} // namespace

void uiSaveSettings()
{
    if (g_loadingSettings || !g_host)
        return;
    const std::string path = settingsPath(kSettingsFile);
    std::FILE* out = path.empty() ? 0 : std::fopen(path.c_str(), "w");
    if (!out)
        return;
    std::fprintf(out, "rem Written by the openbf2-eon64 debug menu.\n");
    std::fprintf(out, "renderer.uiOverlay %d\n", g_overlay ? 1 : 0);
    std::fprintf(out, "renderer.uiConsoleHeight %g\n", uiLayerGetConsoleHeight());
    std::fprintf(out, "renderer.uiMenuKey %d\n", uiLayerGetMenuKey());
    std::fclose(out);
}

namespace {

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
// Menu

float uiScale()
{
    return ImGui::GetFontSize() / 16.0f;
}

std::vector<ConsoleObject*> g_variables;   // renderer set/get bindings, sorted by name
size_t g_variablesSeen = 0;                // registry size they were gathered at
std::map<const ConsoleObject*, std::string> g_firstValues;   // as first shown: what Reset restores

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

bool containsNoCase(const char* text, const char* part)
{
    const size_t n = std::strlen(part);
    if (!n)
        return true;
    for (; *text; ++text)
        if (_strnicmp(text, part, n) == 0)
            return true;
    return false;
}

// "sunShadowCasterReach" shown as "Caster reach" under its prefix's tab.
std::string displayName(const char* name, const char* prefix)
{
    std::string out;
    const size_t skip = prefix ? std::strlen(prefix) : 0;
    for (const char* p = name + skip; *p; ++p)
    {
        const char ch = *p;
        if (out.empty())
            out += static_cast<char>(toupper(ch));
        else if (isupper(static_cast<unsigned char>(ch)) && !isupper(static_cast<unsigned char>(p[-1])))
        {
            out += ' ';
            out += static_cast<char>(tolower(ch));
        }
        else
            out += ch;
    }
    return out;
}

void variableTooltip(ConsoleObject* o, const std::string& type, const std::string& value)
{
    if (!ImGui::BeginItemTooltip())
        return;
    const char* const help = rendererVariableHelp(o->getFunctionName());
    if (help)
        ImGui::TextUnformatted(help);
    ImGui::TextDisabled("renderer.%s", o->getFunctionName());
    if (o->hasRange())
    {
        const std::pair<std::string, std::string> r = o->getRangeAsString();
        ImGui::TextDisabled("%s, %s .. %s, now %s", type.c_str(), r.first.c_str(), r.second.c_str(),
                            value.c_str());
    }
    else
        ImGui::TextDisabled("%s, now %s", type.c_str(), value.c_str());
    ImGui::EndTooltip();
}

// One table row: name, widget, and Reset while the value differs from the one first shown.
void drawVariableRow(ConsoleObject* o, const char* prefix)
{
    const std::string type = o->getReturnType() ? o->getReturnType() : "";
    ImGui::PushID(o);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    const std::string label = displayName(o->getFunctionName(), prefix);
    if (!o->isActive())
    {
        ImGui::TextDisabled("%s", label.c_str());
        variableTooltip(o, type, "-");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("inactive");
        ImGui::TableNextColumn();
        ImGui::PopID();
        return;
    }
    const std::string value = o->execute(0, 0);
    if (g_firstValues.find(o) == g_firstValues.end())
        g_firstValues[o] = value;
    ImGui::TextUnformatted(label.c_str());
    variableTooltip(o, type, value);

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (isType(type, "bool"))
    {
        bool v = value == "1" || value == "true";
        if (ImGui::Checkbox("##v", &v))
            setVariable(o, v ? "1" : "0");
    }
    else if (isType(type, "float", "double") && o->hasRange())
    {
        const std::pair<std::string, std::string> r = o->getRangeAsString();
        float v = static_cast<float>(std::atof(value.c_str()));
        if (ImGui::SliderFloat("##v", &v, static_cast<float>(std::atof(r.first.c_str())),
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
        if (ImGui::SliderInt("##v", &v, std::atoi(r.first.c_str()), std::atoi(r.second.c_str())))
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
        if (ImGui::InputText("##v", text, sizeof(text), ImGuiInputTextFlags_EnterReturnsTrue))
            setVariable(o, text);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        variableTooltip(o, type, value);

    ImGui::TableNextColumn();
    const std::string& first = g_firstValues[o];
    if (value != first)
    {
        if (ImGui::SmallButton("Reset"))
            setVariable(o, first);
        ImGui::SetItemTooltip("Back to %s", first.c_str());
    }
    ImGui::PopID();
}

void drawVariableTable(const char* id, const char* prefix, const char* filter, bool prefixOnly)
{
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
        | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable(id, 3, flags))
        return;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 190.0f * uiScale());
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Reset").x
                                                                           + ImGui::GetStyle().FramePadding.x * 2.0f);
    const size_t prefixLength = std::strlen(prefix);
    for (size_t i = 0; i < g_variables.size(); ++i)
    {
        ConsoleObject* const o = g_variables[i];
        const bool hasPrefix = _strnicmp(o->getFunctionName(), prefix, prefixLength) == 0;
        if (hasPrefix != prefixOnly || !containsNoCase(o->getFunctionName(), filter))
            continue;
        drawVariableRow(o, prefixOnly ? prefix : 0);
    }
    ImGui::EndTable();
}

// Commands ----------------------------------------------------------------------------------

// The arguments of a command line: "{name}" or "{name=default}".
struct Argument
{
    std::string name;
    char value[96];
};

struct Command
{
    std::string section;
    std::string label;
    std::string line;
    std::string help;
    std::vector<Argument> args;
};
std::vector<Command> g_commands;
char g_commandFilter[64] = "";

void parseArguments(Command& c)
{
    size_t at = 0;
    while ((at = c.line.find('{', at)) != std::string::npos)
    {
        const size_t end = c.line.find('}', at);
        if (end == std::string::npos)
            break;
        const std::string spec = c.line.substr(at + 1, end - at - 1);
        const size_t eq = spec.find('=');
        Argument a;
        a.name = eq == std::string::npos ? spec : spec.substr(0, eq);
        std::snprintf(a.value, sizeof(a.value), "%s", eq == std::string::npos ? "" : spec.substr(eq + 1).c_str());
        c.args.push_back(a);
        at = end + 1;
    }
}

// The line with each argument's field in place of its placeholder.
std::string expand(const Command& c)
{
    std::string out;
    size_t at = 0;
    size_t arg = 0;
    while (at < c.line.size())
    {
        const size_t open = c.line.find('{', at);
        const size_t close = open == std::string::npos ? open : c.line.find('}', open);
        if (close == std::string::npos)
        {
            out += c.line.substr(at);
            break;
        }
        out += c.line.substr(at, open - at);
        if (arg < c.args.size())
            out += c.args[arg++].value;
        at = close + 1;
    }
    // Arguments left empty leave no trailing spaces.
    while (!out.empty() && out[out.size() - 1] == ' ')
        out.erase(out.size() - 1);
    return out;
}

void loadCommands()
{
    g_commands.clear();
    std::vector<std::string> lines;
    readLines(settingsPath(kCommandsFile), lines);
    std::string section = "Commands";
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string& l = lines[i];
        if (l.empty() || l[0] == '#')
            continue;
        if (l[0] == '[' && l[l.size() - 1] == ']')
        {
            section = trim(l.substr(1, l.size() - 2));
            continue;
        }
        const size_t eq = l.find('=');
        if (eq == std::string::npos)
            continue;
        Command c;
        c.section = section;
        c.label = trim(l.substr(0, eq));
        std::string rest = l.substr(eq + 1);
        const size_t bar = rest.find(" | ");
        if (bar != std::string::npos)
        {
            c.help = trim(rest.substr(bar + 3));
            rest = rest.substr(0, bar);
        }
        c.line = trim(rest);
        parseArguments(c);
        if (!c.label.empty() && !c.line.empty())
            g_commands.push_back(c);
    }
    std::fprintf(stderr, "[UI] %u menu commands from %s\n", static_cast<unsigned int>(g_commands.size()),
                 settingsPath(kCommandsFile).c_str());
}

bool commandMatches(const Command& c)
{
    return containsNoCase(c.label.c_str(), g_commandFilter) || containsNoCase(c.line.c_str(), g_commandFilter)
        || containsNoCase(c.help.c_str(), g_commandFilter);
}

void commandTooltip(const Command& c)
{
    if (!ImGui::BeginItemTooltip())
        return;
    if (!c.help.empty())
        ImGui::TextUnformatted(c.help.c_str());
    ImGui::TextDisabled("%s", expand(c).c_str());
    ImGui::EndTooltip();
}

// A section: argument-free commands as a grid of equal buttons, then one row per command with
// arguments (its button, then a field per argument).
void drawSection(size_t first, size_t end)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float buttonWidth = 150.0f * uiScale();
    const int columns = std::max(1, static_cast<int>((avail + spacing) / (buttonWidth + spacing)));
    const float width = (avail - spacing * (columns - 1)) / columns;
    int column = 0;
    for (size_t i = first; i < end; ++i)
    {
        Command& c = g_commands[i];
        if (!c.args.empty() || !commandMatches(c))
            continue;
        ImGui::PushID(static_cast<int>(i));
        if (column)
            ImGui::SameLine();
        if (ImGui::Button(c.label.c_str(), ImVec2(width, 0.0f)))
            uiRunTyped(expand(c));
        commandTooltip(c);
        column = (column + 1) % columns;
        ImGui::PopID();
    }
    for (size_t i = first; i < end; ++i)
    {
        Command& c = g_commands[i];
        if (c.args.empty() || !commandMatches(c))
            continue;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(c.label.c_str(), ImVec2(width, 0.0f)))
            uiRunTyped(expand(c));
        commandTooltip(c);
        const float fieldWidth = std::max(60.0f * uiScale(),
            (ImGui::GetContentRegionAvail().x - width - spacing * c.args.size()) / c.args.size());
        for (size_t a = 0; a < c.args.size(); ++a)
        {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(a));
            ImGui::SetNextItemWidth(std::min(fieldWidth, 160.0f * uiScale()));
            if (ImGui::InputTextWithHint("##arg", c.args[a].name.c_str(), c.args[a].value,
                                         sizeof(c.args[a].value), ImGuiInputTextFlags_EnterReturnsTrue))
                uiRunTyped(expand(c));
            ImGui::SetItemTooltip("%s (Enter runs the command)", c.args[a].name.c_str());
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

void drawCommandsTab()
{
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Reload").x
                            - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##cmdfilter", "filter commands", g_commandFilter, sizeof(g_commandFilter));
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
        loadCommands();
    ImGui::SetItemTooltip("Reads %s again", settingsPath(kCommandsFile).c_str());

    if (g_commands.empty())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("No commands. One \"label = console line | help\" per line, \"[Section]\" "
                           "to group, \"{argument}\" or \"{argument=default}\" for a field, in");
        ImGui::TextDisabled("%s", settingsPath(kCommandsFile).c_str());
        return;
    }
    if (!ImGui::BeginChild("##commands", ImVec2(0.0f, 0.0f)))
    {
        ImGui::EndChild();
        return;
    }
    size_t first = 0;
    while (first < g_commands.size())
    {
        size_t end = first;
        bool any = false;
        while (end < g_commands.size() && g_commands[end].section == g_commands[first].section)
            any = commandMatches(g_commands[end++]) || any;
        if (any)
        {
            ImGui::SeparatorText(g_commands[first].section.c_str());
            drawSection(first, end);
        }
        first = end;
    }
    ImGui::EndChild();
}

// Tabs --------------------------------------------------------------------------------------

void drawShadowsTab()
{
    if (ImGui::BeginChild("##shadows", ImVec2(0.0f, 0.0f)))
        drawVariableTable("##sunvars", "sunShadow", "", true);
    ImGui::EndChild();
}

void drawRendererTab()
{
    static char filter[64] = "";
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##varfilter", "filter renderer variables", filter, sizeof(filter));
    if (ImGui::BeginChild("##renderer", ImVec2(0.0f, 0.0f)))
        drawVariableTable("##vars", "sunShadow", filter, false);
    ImGui::EndChild();
}

std::string keyName(int vk)
{
    char name[64] = "";
    const UINT scan = ::MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scan << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT)
        lParam |= 1 << 24;   // the extended keys, not their numeric-pad twins
    if (!::GetKeyNameTextA(lParam, name, sizeof(name)))
        std::snprintf(name, sizeof(name), "key %d", vk);
    return name;
}

void drawSettingsTab()
{
    bool overlay = g_overlay;
    if (ImGui::Checkbox("Performance overlay", &overlay))
        uiLayerSetOverlay(overlay);
    ImGui::SetItemTooltip("renderer.uiOverlay: top right; fps, frame-time graph, GPU passes, draws, memory");

    float height = uiLayerGetConsoleHeight();
    ImGui::SetNextItemWidth(220.0f * uiScale());
    if (ImGui::SliderFloat("Console height", &height, 0.2f, 0.9f, "%.2f"))
        uiLayerSetConsoleHeight(height);
    ImGui::SetItemTooltip("renderer.uiConsoleHeight: the console window's share of the screen height");

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Menu key: %s", keyName(uiLayerGetMenuKey()).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(renderer.uiMenuKey %d)", uiLayerGetMenuKey());

    ImGui::SeparatorText("Files");
    ImGui::TextDisabled("Saved settings  %s", settingsPath(kSettingsFile).c_str());
    ImGui::TextDisabled("Commands        %s", settingsPath(kCommandsFile).c_str());
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
    uiSaveSettings();
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
    ImGui::SetNextWindowSize(ImVec2(640.0f * uiScale(), viewport->WorkSize.y * 0.72f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f * uiScale(), 300.0f * uiScale()),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("openbf2-eon64", 0, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }
    // Header: frame rate, map, and the overlay switch on the right.
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.0f fps", io.Framerate);
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f ms", io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    const char* const map = g_host ? g_host->uiMapName() : "";
    if (map && *map)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s  %s", map, g_host->uiGameMode());
    }
    const char* const overlayLabel = "Overlay";
    const float checkWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x
        + ImGui::CalcTextSize(overlayLabel).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - checkWidth));
    bool overlay = g_overlay;
    if (ImGui::Checkbox(overlayLabel, &overlay))
        uiLayerSetOverlay(overlay);

    gatherVariables();
    if (ImGui::BeginTabBar("##tabs"))
    {
        if (ImGui::BeginTabItem("Commands"))
        {
            drawCommandsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shadows"))
        {
            drawShadowsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Renderer"))
        {
            drawRendererTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings"))
        {
            drawSettingsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
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
