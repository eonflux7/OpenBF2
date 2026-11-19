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
// Camera convention: BF2's camera-to-world matrix (rows right, up, forward, position; y up),
// yaw and pitch in degrees as getRotationZXY reports them (pitch negative looking down), the FOV
// vertical in degrees.

#include "ViewTool.h"

#if !DEDICATED

#include "../../Core/ConsoleObjectMacro.h"
#include "../../IO/Console/Console.h"
#include "../../IO/Input/Input.h"
#include "../../IO/Input/PlayerInput.h"
#include "../../Math/Mat4.h"
#include "../../RendMisc/IRenderView.h"

#include <chrono>
#include <cstring>
#include <string>

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
    bool  recentre;         // the next frame only centres the cursor
    Camera camera;
    std::string savedDrawHud;
    int   wheel;            // wheel notches since the last frame
    std::chrono::steady_clock::time_point lastStep;
    bool  haveLastStep;
};

State g_state = { false, false, false, { 0, 0, 0, 0, 0, 60.0f }, std::string(), 0, {}, false };

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

void restoreHud()
{
    std::string value = g_state.savedDrawHud;
    while (!value.empty() && (value[value.size() - 1] == '\n' || value[value.size() - 1] == ' '))
        value.erase(value.size() - 1);
    runConsole("renderer.drawHud " + (value.empty() ? std::string("1") : value));
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
    else
    {
        // Free fly off leaves the tool, until views hold the camera where fly left it.
        leave();
    }
}

int getFly() { return g_state.fly ? 1 : 0; }

void viewLeave() { leave(); }

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
