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
// changes the FOV. Input is read only while the game window is in the foreground. The HUD and spawn
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
#include "../../Core/SettingsRepostitory.h"
#include "../../IO/Console/Console.h"
#include "../../IO/Input/Input.h"
#include "../../IO/Input/PlayerInput.h"
#include "../../Math/Mat4.h"
#include "../../RendMisc/IRenderView.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>    // after the engine headers: GetAsyncKeyState, the cursor

namespace dice {
namespace hfe {

namespace io { extern IBase* inputDeviceManager; }   // Core/Globals.cpp

bool g_viewToolActive = false;
bool g_viewToolFlying = false;

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
    Camera camera;
    std::string savedDrawHud;
    int   wheel;            // wheel notches since the last frame
    std::chrono::steady_clock::time_point lastStep;
    bool  haveLastStep;
    std::string view;       // the view held, empty when none (free fly from the game's camera)
    std::vector<std::pair<std::string, std::string> > restores;  // variable, value before the view
};

State g_state = { false, false, false, false, { 0, 0, 0, 0, 0, 60.0f }, std::string(), 0, {}, false,
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
    ShowCursor(FALSE);
}

void stopFlying()
{
    if (!g_state.fly)
        return;
    g_state.fly = false;
    g_viewToolFlying = false;
    ShowCursor(TRUE);
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

    HWND window = gameWindow();
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

void viewToolApplyCamera(dx9rend::IRenderView* view)
{
    if (!view)
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
    if (g_state.fly)
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
BF2_CONSOLE_METHOD_0_RET_ACTIVE(list, viewList, std::string, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(reload, viewReload, std::string, true, kViewToolConsoleTag)

// tools.* variables
BF2_CONSOLE_OBJECT("tools", 1, 0)
BF2_CONSOLE_PROPERTY_FN(flySpeed, float, true, g_flySpeed, g_flySpeed =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(flyMouse, float, true, g_flyMouse, g_flyMouse =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(flyKey, int, true, g_flyKey, g_flyKey =, kViewToolConsoleTag)

std::list<ConsoleObject*>& viewToolConsoleObjects()
{
    return consoleBindingList(kViewToolConsoleTag);
}

} // namespace hfe
} // namespace dice

#endif // !DEDICATED
