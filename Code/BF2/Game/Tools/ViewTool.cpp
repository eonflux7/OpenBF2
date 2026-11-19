// [TOOLS] The view tool (docs/eon64/tools.md#1-views): camera override and free fly, tools
// milestone 1. Not part of retail BF2; client image only.
//
// The tool takes the render camera in BF2Render::render after the game has set it and before the
// renderer culls, picks LODs, streams or sets up cascades, so all of those follow the override.
// Only the client's camera moves: no soldier is spawned and the server does not know.
//
// Free fly reads the keyboard and mouse directly, once per rendered frame, so it moves as smoothly
// as the frame rate and also on the spawn screen, whose control map has no movement. While it
// flies, the DirectInput mouse is released (as for the debug menu; the message pump keeps it
// released) and the cursor is hidden and re-centred each frame: its offset is the mouse look. The
// game's player input is cleared in BF2Engine's filter, so neither a soldier nor the spawn menu
// reacts. Keys: W A S D, E or Space up, Q or C down, Shift faster, Ctrl slower; the mouse wheel
// changes the FOV. Input is read only while the game window is in the foreground and neither the
// debug menu nor the console window is shown; meanwhile the cursor shows. The HUD and spawn
// menu are hidden through renderer.drawHud while the tool is active.
//
// Views (task 3): each map's view file, <GSModDirectory>/settings/eon64-views/<map>.txt (ViewFile),
// is reread whenever it changed on disk, so hand edits apply at the next view command. view.go
// holds the camera at a view and runs its `run` lines; a line that sets a console variable records
// the variable's value first, and leaving the view restores it. Lines that call commands are not
// undone. Free fly started from a view stops where it is and keeps holding the camera there; free
// fly started from the game's camera hands it back when it stops.
//
// Camera convention: BF2's camera-to-world matrix (rows right, up, forward, position; y up),
// yaw and pitch in degrees as getRotationZXY reports them (pitch negative looking down), the FOV
// vertical in degrees.

#include "ViewTool.h"

#if !DEDICATED

#include "ViewFile.h"
#include "../IGame.h"
#include "../../Core/ConsoleObject.h"
#include "../../Core/ConsoleObjectMacro.h"
#include "../../Core/ConsoleObjects.h"
#include "../../Core/ClassManager.h"
#include "../../Core/SmartItf.h"
#include "../../IO/File/FileManager.h"
#include "../../Core/SettingsRepostitory.h"
#include "../../IO/Console/Console.h"
#include "../../IO/Input/Input.h"
#include "../../IO/Input/PlayerInput.h"
#include "../../Math/Mat4.h"
#include "../../RendMisc/IRenderView.h"
#include "../../RendMisc/IRendererTools.h"
#include "../../RendMisc/IRendererUi.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>    // after the engine headers: GetAsyncKeyState, the cursor

namespace dice {
namespace hfe {

namespace io { extern IBase* inputDeviceManager; }   // Core/Globals.cpp
bool uiWantsMouse();    // BF2Engine.cpp: the debug menu or the console window is shown

bool g_viewToolActive = false;
bool g_viewToolFlying = false;
bool g_viewToolShooting = false;

namespace {

const char* const kViewToolConsoleTag = "tools.view";

const float kDegToRad = 3.14159265358979f / 180.0f;
const float kMinFov = 5.0f;
const float kMaxFov = 120.0f;
const float kFovStep = 2.0f;          // degrees per wheel notch

struct Camera
{
    float x, y, z;
    float yaw, pitch;   // degrees
    float fov;          // vertical, degrees
};

struct State
{
    bool  fly;
    bool  seedFromGame;     // take the game's camera at the next apply
    bool  seedFovFromGame;  // take only the game's FOV at the next apply (a view without fov=)
    bool  recentre;         // the next frame only centres the cursor
    bool  cursorHidden;
    Camera camera;
    std::string savedDrawHud;
    int   wheel;            // wheel notches since the last frame
    std::chrono::steady_clock::time_point lastStep;
    bool  haveLastStep;
    std::string view;       // the view held, empty when none (free fly from the game's camera)
    std::vector<std::pair<std::string, std::string> > restores;  // variable, value before the view
};

State g_state = { false, false, false, false, false, { 0, 0, 0, 0, 0, 60.0f }, std::string(), 0, {}, false,
                  std::string(), std::vector<std::pair<std::string, std::string> >() };

ViewFile g_views;

// tools.* variables
float g_flySpeed = 20.0f;   // metres per second
float g_flyMouse = 0.15f;   // degrees per pixel of mouse movement
int   g_flyKey   = 0x77;    // VK_F8

std::string runConsole(const std::string& line)
{
    std::string out;
    if (io::mainConsole)
        io::mainConsole->executeLine(line, out, false, false, false, false, false);
    return out;
}

void hideHud()
{
    g_state.savedDrawHud = runConsole("renderer.drawHud");
    runConsole("renderer.drawHud 0");
}

std::string trimEnd(std::string value)
{
    while (!value.empty() && (value[value.size() - 1] == '\n' || value[value.size() - 1] == '\r'
                              || value[value.size() - 1] == ' '))
        value.erase(value.size() - 1);
    return value;
}

void restoreHud()
{
    const std::string value = trimEnd(g_state.savedDrawHud);
    runConsole("renderer.drawHud " + (value.empty() ? std::string("1") : value));
}

// The console variable a run line sets ("renderer.sunShadowDebug 1" sets renderer.sunShadowDebug),
// or empty when the line calls a command or only reads a variable.
std::string variableSetBy(const std::string& line)
{
    std::istringstream in(line);
    std::string name, value;
    if (!(in >> name) || !(in >> value) || !mainConsoleObjects)
        return std::string();
    const size_t dot = name.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size())
        return std::string();
    ConsoleObject* object =
        mainConsoleObjects->getConsoleObject(name.substr(0, dot), name.substr(dot + 1));
    return object && object->isSetGet() ? name : std::string();
}

// Runs a view's line; before a variable is first set, its value is recorded for undoRuns.
std::string runViewLine(const std::string& line)
{
    const std::string variable = variableSetBy(line);
    if (!variable.empty())
    {
        bool recorded = false;
        for (size_t i = 0; i < g_state.restores.size() && !recorded; ++i)
            recorded = g_state.restores[i].first == variable;
        if (!recorded)
            g_state.restores.push_back(std::make_pair(variable, trimEnd(runConsole(variable))));
    }
    return trimEnd(runConsole(line));
}

// Restores the variables the held view's run lines set, last first.
void undoRuns()
{
    for (size_t i = g_state.restores.size(); i-- > 0;)
        runConsole(g_state.restores[i].first + " " + g_state.restores[i].second);
    g_state.restores.clear();
}

// The window that takes the input: the foreground window, when it is this process's.
HWND gameWindow()
{
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    if (!window)
        return 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId() ? window : 0;
}

void startFlying()
{
    if (g_state.fly)
        return;
    g_state.fly = true;
    g_viewToolFlying = true;
    g_state.haveLastStep = false;
    g_state.recentre = true;
    g_state.wheel = 0;
    // Release the DirectInput mouse, as the debug menu does; the message pump keeps it released
    // while g_viewToolFlying and re-acquires it afterwards.
    io::IInputDevice* mouse =
        static_cast<io::IInputDeviceManager*>(io::inputDeviceManager)->getInputDevice(io::IDFMouse);
    if (mouse)
        mouse->deActivate();
}

// The cursor is hidden while free fly reads the mouse, and shown for the menu or the console.
void showCursor(bool show)
{
    if (show == !g_state.cursorHidden)
        return;
    g_state.cursorHidden = !show;
    ShowCursor(show ? TRUE : FALSE);
}

void stopFlying()
{
    if (!g_state.fly)
        return;
    g_state.fly = false;
    g_viewToolFlying = false;
    showCursor(true);
}

void enter()
{
    if (g_viewToolActive)
        return;
    g_viewToolActive = true;
    hideHud();
}

void leave()
{
    if (!g_viewToolActive)
        return;
    g_viewToolActive = false;
    stopFlying();
    undoRuns();
    g_state.view.clear();
    restoreHud();
}

void cameraToMatrix(const Camera& c, Mat4& m)
{
    m = Mat4::getIdentity();
    setRotationZXY(m, 0.0f, c.pitch, c.yaw);
    m.m[12] = c.x;
    m.m[13] = c.y;
    m.m[14] = c.z;
}

void cameraFromView(dx9rend::IRenderView* view, Camera& c)
{
    const Mat4& m = *view->getViewMatrixPtr();      // the camera-to-world matrix the game set
    float roll = 0.0f;
    getRotationZXY(m, roll, c.pitch, c.yaw);
    c.x = m.m[12];
    c.y = m.m[13];
    c.z = m.m[14];
    c.fov = view->getFov() / kDegToRad;
}

void setFly(int on)
{
    if (on)
    {
        if (!g_viewToolActive)
            g_state.seedFromGame = true;
        startFlying();
        enter();
    }
    else if (!g_state.view.empty())
    {
        stopFlying();       // from a view: hold the camera where free fly left it
    }
    else
    {
        leave();            // from the game's camera: hand it back
    }
}

int getFly() { return g_state.fly ? 1 : 0; }

void viewLeave() { leave(); }

// <GSModDirectory>/settings/eon64-views/<map>.txt for the loaded map, or empty with a reason.
std::string viewFilePath(std::string& error)
{
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
    {
        error = "no map loaded";
        return std::string();
    }
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    std::string dir = settings
        ? settings->stringGet(std::string("GSModDirectory"), std::string("mods/bf2"))
        : std::string("mods/bf2");
    if (!dir.empty() && dir[dir.size() - 1] != '/' && dir[dir.size() - 1] != '\\')
        dir += '/';
    return dir + "settings/eon64-views/" + map + ".txt";
}

// Rereads the view file when it changed on disk or the map changed. False with a reason when no
// map is loaded.
bool refreshViews(std::string& error, bool force = false)
{
    const std::string path = viewFilePath(error);
    if (path.empty())
        return false;
    if (force || g_views.changedOnDisk(path))
        g_views.load(path);
    return true;
}

std::string problemsText()
{
    std::string text;
    for (size_t i = 0; i < g_views.problems().size(); ++i)
        text += "\n  " + g_views.problems()[i];
    return text;
}

std::string viewGo(const std::string& name)
{
    std::string error;
    if (!refreshViews(error))
        return "view.go: " + error;
    const ViewDef* def = g_views.find(name);
    if (!def)
        return "view.go: no view " + name + " in " + g_views.path();
    const ViewDef view = *def;      // run lines may reload the file

    undoRuns();                     // the previous view's variables
    stopFlying();
    g_state.seedFromGame = false;
    g_state.camera.x = view.x;
    g_state.camera.y = view.y;
    g_state.camera.z = view.z;
    g_state.camera.yaw = view.yaw;
    g_state.camera.pitch = view.pitch;
    if (view.hasFov)
        g_state.camera.fov = view.fov;
    g_state.seedFovFromGame = !view.hasFov;
    enter();
    g_state.view = view.name;

    std::string text = "view " + view.name;
    if (!view.note.empty())
        text += " | " + view.note;
    for (size_t i = 0; i < view.runs.size(); ++i)
    {
        const std::string out = runViewLine(view.runs[i]);
        text += "\n  run " + view.runs[i] + (out.empty() ? std::string() : "  -> " + out);
    }
    return text;
}

// The camera the tool holds (free fly or a view) becomes view NAME; a note replaces the old one.
std::string viewSave(const std::string& name, const std::string& note = std::string())
{
    if (!g_viewToolActive)
        return "view.save: no camera to save; start free fly (view.fly 1) or go to a view first";
    if (!ViewFile::validName(name))
        return "view.save: a name has no spaces, '|', '#' or '='";
    std::string error;
    if (!refreshViews(error))
        return "view.save: " + error;
    const bool existed = g_views.find(name) != 0;
    const Camera& c = g_state.camera;
    g_views.saveView(name, c.x, c.y, c.z, c.yaw, c.pitch, c.fov, note);
    CreateDirectoryA(g_views.path().substr(0, g_views.path().find_last_of('/')).c_str(), 0);
    if (!g_views.write(error))
        return "view.save: " + error;
    return (existed ? "updated view " : "saved view ") + name + " in " + g_views.path();
}

std::string viewDelete(const std::string& name)
{
    std::string error;
    if (!refreshViews(error))
        return "view.delete: " + error;
    if (!g_views.deleteView(name))
        return "view.delete: no view " + name + " in " + g_views.path();
    if (!g_views.write(error))
        return "view.delete: " + error;
    return "deleted view " + name;
}

std::string viewList()
{
    std::string error;
    if (!refreshViews(error))
        return "view.list: " + error;
    std::ostringstream out;
    out << g_views.path() << (g_views.exists() ? "" : " (no file yet)");
    const std::vector<ViewDef>& views = g_views.views();
    for (size_t i = 0; i < views.size(); ++i)
    {
        const ViewDef& v = views[i];
        out << "\n" << (v.name == g_state.view ? "* " : "  ") << v.name << "  pos="
            << ViewFile::formatNumber(v.x, 2) << "," << ViewFile::formatNumber(v.y, 2) << ","
            << ViewFile::formatNumber(v.z, 2) << " yaw=" << ViewFile::formatNumber(v.yaw, 2)
            << " pitch=" << ViewFile::formatNumber(v.pitch, 2);
        if (v.hasFov)
            out << " fov=" << ViewFile::formatNumber(v.fov, 1);
        if (!v.note.empty())
            out << " | " << v.note;
        for (size_t r = 0; r < v.runs.size(); ++r)
            out << "\n      run " << v.runs[r]
                << (variableSetBy(v.runs[r]).empty() ? "  (command, not undone)" : "");
    }
    const std::vector<ViewSet>& sets = g_views.sets();
    for (size_t i = 0; i < sets.size(); ++i)
    {
        out << "\n  set " << sets[i].name << ":";
        for (size_t v = 0; v < sets[i].views.size(); ++v)
            out << " " << sets[i].views[v];
    }
    return out.str() + problemsText();
}

std::string viewReload()
{
    std::string error;
    if (!refreshViews(error, true))
        return "view.reload: " + error;
    std::ostringstream out;
    out << g_views.path() << ": " << g_views.views().size() << " views, " << g_views.sets().size()
        << " sets" << (g_views.exists() ? "" : " (no file yet)");
    return out.str() + problemsText();
}

// Writes the file after an edit; "" when it was written, else the reason.
std::string writeViews()
{
    std::string error;
    return g_views.write(error) ? std::string() : error;
}

// Moves view NAME one place up (-1, "up") or down (1, "down") in the file, and so in the list.
std::string viewMove(const std::string& name, const std::string& direction)
{
    const int delta = direction == "up" || direction == "-1" ? -1
                    : direction == "down" || direction == "1" ? 1 : 0;
    if (delta == 0)
        return "view.move: direction is up or down";
    std::string error;
    if (!refreshViews(error))
        return "view.move: " + error;
    if (!g_views.find(name))
        return "view.move: no view " + name + " in " + g_views.path();
    if (!g_views.moveView(name, delta))
        return "view.move: " + name + " is already " + (delta < 0 ? "first" : "last");
    error = writeViews();
    return error.empty() ? "moved view " + name + " " + (delta < 0 ? "up" : "down")
                         : "view.move: " + error;
}

// Replaces view NAME's note; no note removes it.
std::string viewNote(const std::string& name, const std::string& note = std::string())
{
    std::string error;
    if (!refreshViews(error))
        return "view.note: " + error;
    if (!g_views.setNote(name, note))
        return "view.note: no view " + name + " in " + g_views.path();
    error = writeViews();
    return error.empty() ? (note.empty() ? "removed the note of " : "set the note of ") + name
                         : "view.note: " + error;
}

// Set NAME becomes the views listed (one argument, quoted when there are several); no views
// removes the set.
std::string viewSet(const std::string& name, const std::string& list = std::string())
{
    if (!ViewFile::validName(name))
        return "view.set: a name has no spaces, '|', '#' or '='";
    std::string error;
    if (!refreshViews(error))
        return "view.set: " + error;
    std::vector<std::string> views;
    std::istringstream in(list);
    std::string view;
    while (in >> view)
    {
        if (!g_views.find(view))
            return "view.set: no view " + view + " in " + g_views.path();
        views.push_back(view);
    }
    bool existed = false;
    for (size_t i = 0; i < g_views.sets().size(); ++i)
        existed = existed || g_views.sets()[i].name == name;
    if (views.empty() && !existed)
        return "view.set: no set " + name;
    g_views.setSet(name, views);
    CreateDirectoryA(g_views.path().substr(0, g_views.path().find_last_of('/')).c_str(), 0);
    error = writeViews();
    if (!error.empty())
        return "view.set: " + error;
    return (views.empty() ? "removed set " : existed ? "updated set " : "added set ") + name;
}

// view.shot (task 5): the request waits for the next frame's hook, which asks the renderer for a
// shot of that frame (IRendererTools.h); a later hook collects the result. tools.shotHud shows
// the HUD for the shot, also while a view hides it, and hides it again afterwards.
struct Shot
{
    bool        queued;     // waiting for the hook to request it
    bool        requested;  // the renderer has the request
    bool        hud;
    bool        hudShown;   // the HUD was shown for this shot and is hidden again afterwards
    int         frames;     // hooks since queued or requested
    std::string png;
    std::string jpg;
    std::string last;       // the last shot's result, for view.lastShot
};

Shot g_shot = { false, false, false, false, 0, std::string(), std::string(), std::string() };

std::string g_outputDir("eon64-runs");   // under the game's working directory
int g_shotHud = 0;
int g_shotWidth = 960;      // the JPEG's width

const int kShotHudFrames = 2;       // frames the HUD draws before its shot
const int kShotTimeoutFrames = 60;  // frames a request may wait for the capture point

dx9rend::IRendererTools* rendererTools()
{
    static dx9rend::IRendererTools* tools = 0;
    if (!tools && classManager)
    {
        IBase* renderer = classManager->getSingleton("Renderer");
        if (renderer)
            tools = static_cast<dx9rend::IRendererTools*>(
                renderer->queryInterface(dx9rend::IID_IRendererTools));
    }
    return tools;
}

// tools.outputDir (@HOME@ allowed) made absolute, with backslashes and no trailing separator.
std::string outputDirectory()
{
    std::string dir = g_outputDir;
    static const std::string home("@HOME@");
    if (dir.compare(0, home.size(), home) == 0 && io::fileManager)
        dir = io::fileManager->getHomeSubstitution() + dir.substr(home.size());
    for (size_t i = 0; i < dir.size(); ++i)
        if (dir[i] == '/')
            dir[i] = '\\';
    char full[MAX_PATH];
    const DWORD n = GetFullPathNameA(dir.c_str(), MAX_PATH, full, 0);
    if (n > 0 && n < MAX_PATH)
        dir = full;
    while (dir.size() > 3 && dir[dir.size() - 1] == '\\')
        dir.erase(dir.size() - 1);
    return dir;
}

// Creates the directory and its parents; false when it does not exist afterwards.
bool makeDirectories(const std::string& dir)
{
    for (size_t i = 0; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '\\')
        {
            const std::string part = dir.substr(0, i);
            if (part.size() > 2)    // past "C:"
                CreateDirectoryA(part.c_str(), 0);
        }
    const DWORD attributes = GetFileAttributesA(dir.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool validShotName(const std::string& name)
{
    if (name.empty() || name[0] == '.')
        return false;
    for (size_t i = 0; i < name.size(); ++i)
        if (!isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_' && name[i] != '-'
            && name[i] != '.')
            return false;
    return true;
}

void shotFinished(const std::string& result)
{
    g_shot.queued = false;
    g_shot.requested = false;
    g_shot.last = result;
    if (g_shot.hudShown && g_viewToolActive)
        runConsole("renderer.drawHud 0");
    g_shot.hudShown = false;
    if (io::mainConsole)
        io::mainConsole->output(result + "\n");
}

// view.shot [NAME]: <outputDir>\<map>\shots\<NAME>.png and .jpg. NAME defaults to the held view's
// name (or "camera") and the local time; an existing NAME is overwritten.
std::string viewShot(const std::string& requested)
{
    if (g_shot.queued || g_shot.requested)
        return "view.shot: a shot is still pending (" + g_shot.png + ")";
    if (!rendererTools())
        return "view.shot: the renderer does not take shots";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "view.shot: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    std::string name = requested;
    if (name.empty())
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay,
                      t.wHour, t.wMinute, t.wSecond);
        name = (g_state.view.empty() ? std::string("camera") : g_state.view) + stamp;
    }
    if (!validShotName(name))
        return "view.shot: a name has only letters, digits, '_', '-' and '.'";
    const std::string dir = outputDirectory() + "\\" + map + "\\shots";
    if (!makeDirectories(dir))
        return "view.shot: cannot create " + dir;
    g_shot.png = dir + "\\" + name + ".png";
    g_shot.jpg = dir + "\\" + name + ".jpg";
    g_shot.hud = g_shotHud != 0;
    g_shot.hudShown = false;
    if (g_shot.hud && g_viewToolActive)
    {
        const std::string value = trimEnd(g_state.savedDrawHud);
        runConsole("renderer.drawHud " + (value.empty() ? std::string("1") : value));
        g_shot.hudShown = true;
    }
    g_shot.frames = 0;
    g_shot.queued = true;
    g_viewToolShooting = true;
    return "view.shot: queued " + g_shot.png;
}

std::string viewLastShot()
{
    if (g_shot.queued || g_shot.requested)
        return "pending " + g_shot.png;
    return g_shot.last.empty() ? std::string("none") : g_shot.last;
}

// Once per frame while a shot is queued or requested, before the frame draws.
void shotStep()
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
    {
        shotFinished("view.shot: failed: the renderer does not take shots");
        g_viewToolShooting = false;
        return;
    }
    if (g_shot.queued)
    {
        if (++g_shot.frames < (g_shot.hud ? kShotHudFrames : 1))
            return;
        tools->toolsRequestShot(g_shot.png.c_str(), g_shot.jpg.c_str(), g_shotWidth, g_shot.hud);
        g_shot.queued = false;
        g_shot.requested = true;
        g_shot.frames = 0;
        return;
    }
    const char* message = "";
    const int state = tools->toolsShotState(message);
    if (state == 0)
    {
        if (++g_shot.frames < kShotTimeoutFrames)
            return;
        tools->toolsRequestShot(0, 0, 0, false);    // cancels it
        shotFinished("view.shot: failed: no frame reached the capture point");
    }
    else if (state > 0)
        shotFinished("view.shot: wrote " + g_shot.png + " (" + message + ")");
    else
        shotFinished("view.shot: failed: " + std::string(message ? message : ""));
    g_viewToolShooting = false;
}

// The Views tab's read-only window on the tool (IRendererUi.h).
class UiViewTool : public dx9rend::IUiViewTool
{
public:
    UiViewTool() : m_haveCheck(false) { }

    virtual void uiViewsRefresh()
    {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (m_haveCheck && now - m_lastCheck < std::chrono::milliseconds(500))
            return;
        m_haveCheck = true;
        m_lastCheck = now;
        std::string error;
        m_path = refreshViews(error) ? g_views.path() : std::string();
    }
    virtual const char* uiViewsPath() const { return m_path.c_str(); }
    virtual bool uiViewsFileExists() const { return !m_path.empty() && g_views.exists(); }
    virtual int uiViewCount() const
    {
        return m_path.empty() ? 0 : static_cast<int>(g_views.views().size());
    }
    virtual bool uiViewAt(int index, dx9rend::UiView& out) const
    {
        if (index < 0 || index >= uiViewCount())
            return false;
        const ViewDef& v = g_views.views()[index];
        out.name = v.name.c_str();
        out.note = v.note.c_str();
        out.x = v.x;
        out.y = v.y;
        out.z = v.z;
        out.yaw = v.yaw;
        out.pitch = v.pitch;
        out.fov = v.fov;
        out.hasFov = v.hasFov;
        out.runCount = static_cast<int>(v.runs.size());
        return true;
    }
    virtual const char* uiViewRun(int view, int run, bool& undone) const
    {
        undone = false;
        if (view < 0 || view >= uiViewCount() || run < 0
            || run >= static_cast<int>(g_views.views()[view].runs.size()))
            return "";
        const std::string& line = g_views.views()[view].runs[run];
        undone = !variableSetBy(line).empty();
        return line.c_str();
    }
    virtual int uiSetCount() const
    {
        return m_path.empty() ? 0 : static_cast<int>(g_views.sets().size());
    }
    virtual const char* uiSetName(int set) const
    {
        return set >= 0 && set < uiSetCount() ? g_views.sets()[set].name.c_str() : "";
    }
    virtual int uiSetViewCount(int set) const
    {
        return set >= 0 && set < uiSetCount() ? static_cast<int>(g_views.sets()[set].views.size())
                                              : 0;
    }
    virtual const char* uiSetView(int set, int index) const
    {
        return index >= 0 && index < uiSetViewCount(set) ? g_views.sets()[set].views[index].c_str()
                                                         : "";
    }
    virtual int uiViewProblemCount() const
    {
        return m_path.empty() ? 0 : static_cast<int>(g_views.problems().size());
    }
    virtual const char* uiViewProblem(int index) const
    {
        return index >= 0 && index < uiViewProblemCount() ? g_views.problems()[index].c_str() : "";
    }
    virtual bool uiViewActive() const { return g_viewToolActive; }
    virtual bool uiViewFlying() const { return g_state.fly; }
    virtual const char* uiHeldView() const { return g_state.view.c_str(); }

private:
    std::string m_path;
    bool m_haveCheck;
    std::chrono::steady_clock::time_point m_lastCheck;
};

UiViewTool g_uiViewTool;

inline bool keyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

// One frame of free fly: keys and the mouse's offset from the window centre, over the wall-clock
// time since the last frame.
void flyStep()
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (g_state.haveLastStep)
        dt = std::chrono::duration<float>(now - g_state.lastStep).count();
    g_state.lastStep = now;
    g_state.haveLastStep = true;
    if (dt > 0.1f)
        dt = 0.1f;

    Camera& c = g_state.camera;
    c.fov -= g_state.wheel * kFovStep;
    g_state.wheel = 0;
    if (c.fov < kMinFov) c.fov = kMinFov;
    if (c.fov > kMaxFov) c.fov = kMaxFov;

    // The keys and the mouse belong to the debug menu or the console while either is shown.
    HWND window = uiWantsMouse() ? 0 : gameWindow();
    showCursor(!window);
    if (!window)
    {
        g_state.recentre = true;
        return;
    }

    RECT client;
    GetClientRect(window, &client);
    POINT centre = { (client.right - client.left) / 2, (client.bottom - client.top) / 2 };
    ClientToScreen(window, &centre);
    POINT cursor;
    GetCursorPos(&cursor);
    SetCursorPos(centre.x, centre.y);
    if (g_state.recentre)
    {
        g_state.recentre = false;
        return;
    }

    c.yaw   -= (cursor.x - centre.x) * g_flyMouse;
    c.pitch -= (cursor.y - centre.y) * g_flyMouse;
    if (c.pitch > 89.0f)  c.pitch = 89.0f;
    if (c.pitch < -89.0f) c.pitch = -89.0f;
    while (c.yaw > 180.0f)   c.yaw -= 360.0f;
    while (c.yaw <= -180.0f) c.yaw += 360.0f;

    float speed = g_flySpeed;
    if (keyDown(VK_SHIFT))
        speed *= 5.0f;
    else if (keyDown(VK_CONTROL))
        speed *= 0.2f;
    const float step = speed * dt;

    const float forward = (keyDown('W') ? 1.0f : 0.0f) - (keyDown('S') ? 1.0f : 0.0f);
    const float strafe  = (keyDown('D') ? 1.0f : 0.0f) - (keyDown('A') ? 1.0f : 0.0f);
    const float up      = (keyDown('E') || keyDown(VK_SPACE) ? 1.0f : 0.0f)
                        - (keyDown('Q') || keyDown('C') ? 1.0f : 0.0f);

    Mat4 m;
    cameraToMatrix(c, m);
    c.x += (m.m[8] * forward + m.m[0] * strafe) * step;
    c.y += (m.m[9] * forward + m.m[1] * strafe + up) * step;
    c.z += (m.m[10] * forward + m.m[2] * strafe) * step;
}

} // namespace

void viewToolFrame(dx9rend::IRenderView* view)
{
    if (g_viewToolShooting)
        shotStep();
    if (!view || !g_viewToolActive)
        return;
    if (g_state.seedFromGame)
    {
        cameraFromView(view, g_state.camera);
        g_state.seedFromGame = false;
    }
    if (g_state.seedFovFromGame)
    {
        g_state.camera.fov = view->getFov() / kDegToRad;
        g_state.seedFovFromGame = false;
    }
    if (g_state.fly)
        flyStep();
    Mat4 m;
    cameraToMatrix(g_state.camera, m);
    view->setFov(g_state.camera.fov * kDegToRad);
    view->setViewMatrix(m.m);
}

// During free fly the game's player input is cleared, so neither a soldier nor the spawn menu
// moves; the camera reads the devices itself (flyStep).
bool viewToolFilterPlayerInput(io::PlayerInput& input)
{
    if (!g_state.fly)
        return true;
    std::memset(input.inputs, 0, sizeof(input.inputs));
    return false;
}

void viewToolMouseWheel(int delta)
{
    if (g_state.fly && !uiWantsMouse())    // the menu scrolls with it
        g_state.wheel += delta / WHEEL_DELTA;
}

void viewToolKeyDown(unsigned int virtualKey)
{
    if (g_flyKey != 0 && virtualKey == static_cast<unsigned int>(g_flyKey))
        setFly(g_state.fly ? 0 : 1);
}

// view.* commands
BF2_CONSOLE_OBJECT("view", 1, 0)
BF2_CONSOLE_PROPERTY_FN(fly, int, true, getFly(), setFly, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_STATIC_ACTIVE(leave, viewLeave, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_RET_ACTIVE(go, viewGo, std::string, true, std::string, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_2_RET_ACTIVE(save, viewSave, std::string, true, std::string, std::string,
                                  kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_RET_ACTIVE(delete, viewDelete, std::string, true, std::string,
                                kViewToolConsoleTag)
BF2_CONSOLE_METHOD_2_RET_ACTIVE(move, viewMove, std::string, true, std::string, std::string,
                                kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_2_RET_ACTIVE(note, viewNote, std::string, true, std::string, std::string,
                                  kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_2_RET_ACTIVE(set, viewSet, std::string, true, std::string, std::string,
                                  kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(list, viewList, std::string, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(reload, viewReload, std::string, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_1_RET_ACTIVE(shot, viewShot, std::string, true, std::string, std::string(),
                                 kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(lastShot, viewLastShot, std::string, true, kViewToolConsoleTag)

// tools.* variables
BF2_CONSOLE_OBJECT("tools", 1, 0)
BF2_CONSOLE_PROPERTY_FN(flySpeed, float, true, g_flySpeed, g_flySpeed =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(flyMouse, float, true, g_flyMouse, g_flyMouse =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(flyKey, int, true, g_flyKey, g_flyKey =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(outputDir, std::string, true, g_outputDir, g_outputDir =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(shotHud, int, true, g_shotHud, g_shotHud =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(shotWidth, int, true, g_shotWidth, g_shotWidth =, kViewToolConsoleTag)

dx9rend::IUiViewTool* viewToolUi()
{
    return &g_uiViewTool;
}

std::list<ConsoleObject*>& viewToolConsoleObjects()
{
    return consoleBindingList(kViewToolConsoleTag);
}

} // namespace hfe
} // namespace dice

#endif // !DEDICATED
