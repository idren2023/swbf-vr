// Pointer.cpp: the right controller as a pointer in menus.
//
// Menus are a flat screen in front of the player (Xr.cpp, Xr::SubmitFlatFrame) or, inside a match before spawning, the
// floating HUD panel (Stereo.cpp). Where the controller's ray meets that screen is where the game's own mouse
// cursor is sent; the trigger is the left mouse button (Controls.cpp). No pointer beam is drawn: the game's
// cursor is the dot.
//
// The game only takes relative mouse movement and the cursor position cannot be read, so the mod keeps its own
// count: when pointing starts, the cursor is pushed into the top left corner (it stops at the edge), and from then
// on every movement is known. Using the real mouse in between makes the count wrong until pointing starts again
// (point away from the screen and back).

#include "Pointer.h"

#include "FpGuard.h"
#include "Input.h"
#include "Log.h"
#include "Stereo.h"
#include "Xr.h"

#include <windows.h>

#include <cmath>

namespace SwbfVr
{

namespace Pointer
{

namespace
{

// Mouse counts from the left edge to the right edge and from top to bottom of the menu screen. Measured with
// idren2023's mouse sensitivity (see docs/NOTES.md, pointer calibration); swbf-vr.ini can override.
float _screenCountsX = 1022.0f,
      _screenCountsY = 667.0f;            // run57/run58: targets and where the cursor landed agree within about 1 %
constexpr long MAXIMUM_STEP_COUNTS = 400; // counts per frame and axis
constexpr long CORNER_PUSH_COUNTS = 4000; // counts into the corner when pointing starts
/// Accepted range of SetScreenCounts.
constexpr int MINIMUM_SCREEN_COUNTS = 100;
constexpr int MAXIMUM_SCREEN_COUNTS = 20000;
/// The ray counts as pointing at the screen up to this far outside it (fractions of the screen).
constexpr float POINTING_MINIMUM = -0.05f;
constexpr float POINTING_MAXIMUM = 1.05f;
/// Share of the remaining distance the smoothed target moves per frame.
constexpr float SMOOTHING_FACTOR = 0.35f;

enum class Phase
{
    NotPointing,
    PushingIntoCorner,
    Following
};

bool _isOn = true;
Phase _phase = Phase::NotPointing;
long _cornerPushCountsLeft = 0;
float _cursorX = 0, _cursorY = 0; // where the cursor is, in counts from the top left corner
float _smoothU = 0, _smoothV = 0;
bool _isTestOn = false; // script: a fixed target instead of the controller
float _testU = 0, _testV = 0;
bool _isActive = false;

long ClampStep(float counts)
{
    return counts > MAXIMUM_STEP_COUNTS    ? MAXIMUM_STEP_COUNTS
           : counts < -MAXIMUM_STEP_COUNTS ? -MAXIMUM_STEP_COUNTS
                                           : static_cast<long>(counts);
}

} // namespace

bool IsActive()
{
    return _isActive;
}

bool GetCursor(float* u, float* v)
{
    if (!_isActive || _phase != Phase::Following)
    {
        return false;
    }
    *u = _cursorX / _screenCountsX;
    *v = _cursorY / _screenCountsY;
    return true;
}

void SetEnabled(bool isOn)
{
    _isOn = isOn;
    Log::Info("pointer", "menu pointer %s", isOn ? "on" : "off");
}

void SetScreenCounts(float countsX, float countsY)
{
    if (countsX > MINIMUM_SCREEN_COUNTS && countsX < MAXIMUM_SCREEN_COUNTS)
    {
        _screenCountsX = countsX;
    }
    if (countsY > MINIMUM_SCREEN_COUNTS && countsY < MAXIMUM_SCREEN_COUNTS)
    {
        _screenCountsY = countsY;
    }
    Log::Info(
        "pointer",
        "the menu screen is %.0f x %.0f mouse counts",
        static_cast<double>(_screenCountsX),
        static_cast<double>(_screenCountsY));
}

// Script: send the cursor to this point of the screen (fractions, from the top left); negative u ends the test.
void SetTestTarget(float u, float v)
{
    _isTestOn = u >= 0;
    _testU = u;
    _testV = v;
    _phase = Phase::NotPointing;
    Log::Info(
        "pointer",
        "test target %.2f %.2f%s",
        static_cast<double>(u),
        static_cast<double>(v),
        _isTestOn ? "" : " (off)");
}

// Main thread, once per presented frame.
void OnPresent()
{
    FpGuard guard;
    float u = 0, v = 0;
    bool isPointing = false;
    if (_isTestOn)
    {
        u = _testU;
        v = _testV;
        isPointing = true;
    }
    else if (_isOn && Xr::IsRunning())
    {
        float hand[16];
        if (Xr::GetHandPose(0, hand) && Stereo::ComputePointerUV(hand, &u, &v))
        {
            isPointing = u > POINTING_MINIMUM && u < POINTING_MAXIMUM && v > POINTING_MINIMUM && v < POINTING_MAXIMUM;
        }
    }
    if (!isPointing)
    {
        if (_isActive)
        {
            Log::Info("pointer", "off the menu screen");
        }
        _isActive = false;
        _phase = Phase::NotPointing;
        return;
    }
    if (u < 0)
    {
        u = 0;
    }
    if (u > 1)
    {
        u = 1;
    }
    if (v < 0)
    {
        v = 0;
    }
    if (v > 1)
    {
        v = 1;
    }
    if (_phase == Phase::NotPointing)
    {
        _phase = Phase::PushingIntoCorner;
        _cornerPushCountsLeft = CORNER_PUSH_COUNTS;
        _smoothU = u;
        _smoothV = v;
        if (!_isActive)
        {
            Log::Info(
                "pointer",
                "on the menu screen at %.2f %.2f, cursor to the corner first",
                static_cast<double>(u),
                static_cast<double>(v));
        }
        _isActive = true;
    }
    if (_phase == Phase::PushingIntoCorner)
    {
        const long step = _cornerPushCountsLeft > MAXIMUM_STEP_COUNTS ? MAXIMUM_STEP_COUNTS : _cornerPushCountsLeft;
        Input::MouseMove(-step, -step);
        _cornerPushCountsLeft -= step;
        if (_cornerPushCountsLeft <= 0)
        {
            _phase = Phase::Following;
            _cursorX = _cursorY = 0;
        }
        return;
    }
    // A little smoothing: a hand is never still, and menu entries are small.
    _smoothU += (u - _smoothU) * SMOOTHING_FACTOR;
    _smoothV += (v - _smoothV) * SMOOTHING_FACTOR;
    const long stepX = ClampStep(_smoothU * _screenCountsX - _cursorX),
               stepY = ClampStep(_smoothV * _screenCountsY - _cursorY);
    if (stepX || stepY)
    {
        Input::MouseMove(stepX, stepY);
        _cursorX += static_cast<float>(stepX);
        _cursorY += static_cast<float>(stepY);
    }
}

} // namespace Pointer

} // namespace SwbfVr
