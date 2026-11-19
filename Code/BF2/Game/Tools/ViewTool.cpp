// [TOOLS] The view tool (docs/eon64/tools.md#1-views): camera override and free fly, tools
// milestone 1. Not part of retail BF2; client image only.
//
// The tool takes the render camera in BF2Render::render after the game has set it and before the
// renderer culls, picks LODs, streams or sets up cascades, so all of those follow the override.
// Only the client's camera moves: no soldier is spawned and the server does not know.
//
// Free fly reads the game's own player input (the player's bindings and mouse sensitivity) in
// BF2Engine's input filter, which runs before the HUD's, and clears the sample, so neither a
// soldier nor the spawn menu sees it. Moves: throttle forward and back, yaw strafes, lie up, crouch
// down; sprint is faster and alt-sprint slower; the next and previous item (mouse wheel) change
// the FOV. The HUD and spawn menu are hidden through renderer.drawHud while the tool is active.
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

namespace dice {
namespace hfe {

bool g_viewToolActive = false;

namespace {

const char* const kViewToolConsoleTag = "tools.view";

const float kDegToRad = 3.14159265358979f / 180.0f;
const float kMinFov = 5.0f;
const float kMaxFov = 120.0f;
const float kFovStep = 2.0f;          // degrees per wheel notch
const float kLookRate = 100.0f;       // degrees per unit of look axis per second, as FreeCamera

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
    Camera camera;
    std::string savedDrawHud;
    std::chrono::steady_clock::time_point lastInput;
    bool  haveLastInput;
};

State g_state = { false, false, { 0, 0, 0, 0, 0, 60.0f }, std::string(), {}, false };

// tools.* variables
float g_flySpeed = 20.0f;   // metres per second
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
    g_state.fly = false;
    g_state.haveLastInput = false;
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
        g_state.fly = true;
        g_state.haveLastInput = false;
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

inline bool pressed(const io::PlayerInput& input, io::PlayerInputMap index)
{
    return (input.mappingFlags & (1ULL << index)) != 0 && input.inputs[index] > 0.5f;
}

inline float axis(const io::PlayerInput& input, io::PlayerInputMap index)
{
    return (input.mappingFlags & (1ULL << index)) ? input.inputs[index] : 0.0f;
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
    Mat4 m;
    cameraToMatrix(g_state.camera, m);
    view->setFov(g_state.camera.fov * kDegToRad);
    view->setViewMatrix(m.m);
}

bool viewToolFilterPlayerInput(io::PlayerInput& input)
{
    if (!g_state.fly)
        return true;

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (g_state.haveLastInput)
        dt = std::chrono::duration<float>(now - g_state.lastInput).count();
    g_state.lastInput = now;
    g_state.haveLastInput = true;
    if (dt > 0.1f)
        dt = 0.1f;

    Camera& c = g_state.camera;

    c.yaw   += axis(input, io::PIMouseLookX) * dt * kLookRate;
    c.pitch -= axis(input, io::PIMouseLookY) * dt * kLookRate;
    if (c.pitch > 89.0f)  c.pitch = 89.0f;
    if (c.pitch < -89.0f) c.pitch = -89.0f;
    while (c.yaw > 180.0f)   c.yaw -= 360.0f;
    while (c.yaw <= -180.0f) c.yaw += 360.0f;

    if (pressed(input, io::PINextItem)) c.fov -= kFovStep;
    if (pressed(input, io::PIPrevItem)) c.fov += kFovStep;
    if (c.fov < kMinFov) c.fov = kMinFov;
    if (c.fov > kMaxFov) c.fov = kMaxFov;

    float speed = g_flySpeed;
    if (pressed(input, io::PISprint))
        speed *= 5.0f;
    else if (pressed(input, io::PIAltSprint))
        speed *= 0.2f;
    const float step = speed * dt;

    Mat4 m;
    cameraToMatrix(c, m);
    const float forward = axis(input, io::PIThrottle) * step;
    const float strafe  = axis(input, io::PIYaw) * step;
    c.x += m.m[8] * forward + m.m[0] * strafe;
    c.y += m.m[9] * forward + m.m[1] * strafe;
    c.z += m.m[10] * forward + m.m[2] * strafe;
    if (pressed(input, io::PILie))
        c.y += step;
    else if (pressed(input, io::PICrouch))
        c.y -= step;

    std::memset(input.inputs, 0, sizeof(input.inputs));
    return false;
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
BF2_CONSOLE_PROPERTY_FN(flyKey, int, true, g_flyKey, g_flyKey =, kViewToolConsoleTag)

std::list<ConsoleObject*>& viewToolConsoleObjects()
{
    return consoleBindingList(kViewToolConsoleTag);
}

} // namespace hfe
} // namespace dice

#endif // !DEDICATED
