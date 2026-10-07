/*
    This source file is part of Rigs of Rods
    Copyright 2026 Rigs of Rods contributors

    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.

    Rigs of Rods is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Rigs of Rods. If not, see <http://www.gnu.org/licenses/>.
*/

// Built only for ROR_PLATFORM_UWP (see source/main/CMakeLists.txt).

#include "XboxInput.h"

#include "Application.h"
#include "UwpApp.h"
#include "imgui.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Gaming.Input.ForceFeedback.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Gaming::Input;
using namespace winrt::Windows::Gaming::Input::ForceFeedback;

namespace RoR {
namespace XboxInput {

// =================================================================================================
// Device tracker (hot-plug). WGI events arrive on worker threads -> mutex.
// NOTE: right after launch Gamepad::Gamepads() is often still empty; devices show up through the
// *Added events a few ms later. That is why joysticks below are "slots" that read whatever device
// is currently present instead of binding to a device at creation time.
// =================================================================================================

namespace {

struct Tracker
{
    std::mutex               mutex;
    std::vector<Gamepad>     pads;
    std::vector<RacingWheel> wheels;
    event_token              t_pad_add{}, t_pad_rem{}, t_wheel_add{}, t_wheel_rem{};
    bool                     started = false;
};

Tracker& T() { static Tracker t; return t; }

template <typename V, typename D> void AddUnique(V& v, D const& d)
{
    if (std::find(v.begin(), v.end(), d) == v.end()) v.push_back(d);
}
template <typename V, typename D> void Remove(V& v, D const& d)
{
    v.erase(std::remove(v.begin(), v.end(), d), v.end());
}

Gamepad FirstGamepad()
{
    std::lock_guard<std::mutex> lock(T().mutex);
    return T().pads.empty() ? Gamepad(nullptr) : T().pads.front();
}

RacingWheel FirstWheel()
{
    std::lock_guard<std::mutex> lock(T().mutex);
    return T().wheels.empty() ? RacingWheel(nullptr) : T().wheels.front();
}

int StickToAxis(double v)
{
    v = std::max(-1.0, std::min(1.0, v));
    return (int)std::lround(v * (v < 0 ? -(double)OIS::JoyStick::MIN_AXIS : (double)OIS::JoyStick::MAX_AXIS));
}

/// G27-style pedal: released = +MAX_AXIS, fully pressed = MIN_AXIS (use 'REVERSE' in .map files).
int PedalToAxis(double p)
{
    p = std::max(0.0, std::min(1.0, p));
    const double range = (double)OIS::JoyStick::MAX_AXIS - (double)OIS::JoyStick::MIN_AXIS;
    return (int)std::lround((double)OIS::JoyStick::MAX_AXIS - p * range);
}

int PovFromBits(bool up, bool down, bool left, bool right)
{
    int dir = OIS::Pov::Centered;
    if (up)    dir |= OIS::Pov::North;
    if (down)  dir |= OIS::Pov::South;
    if (left)  dir |= OIS::Pov::West;
    if (right) dir |= OIS::Pov::East;
    return dir;
}

/// Compare old/new states and fire OIS buffered events, then commit the new state.
void CommitAndNotify(OIS::JoyStick* dev, OIS::JoyStickState& cur, OIS::JoyStickState const& next,
                     OIS::JoyStickListener* listener, bool buffered)
{
    if (!buffered || !listener)
    {
        cur = next;
        return;
    }
    OIS::JoyStickState old = cur;
    cur = next;
    OIS::JoyStickEvent ev(dev, cur);
    for (size_t i = 0; i < cur.mButtons.size() && i < old.mButtons.size(); ++i)
    {
        if (cur.mButtons[i] != old.mButtons[i])
        {
            if (cur.mButtons[i]) { if (!listener->buttonPressed(ev, (int)i)) return; }
            else                 { if (!listener->buttonReleased(ev, (int)i)) return; }
        }
    }
    for (size_t i = 0; i < cur.mAxes.size() && i < old.mAxes.size(); ++i)
    {
        if (cur.mAxes[i].abs != old.mAxes[i].abs)
        {
            if (!listener->axisMoved(ev, (int)i)) return;
        }
    }
    if (cur.mPOV[0].direction != old.mPOV[0].direction)
    {
        listener->povMoved(ev, 0);
    }
}

} // anonymous namespace

void Startup()
{
    Tracker& t = T();
    if (t.started)
        return;
    t.started = true;

    t.t_pad_add = Gamepad::GamepadAdded([](IInspectable const&, Gamepad const& g)
    {
        std::lock_guard<std::mutex> lock(T().mutex);
        AddUnique(T().pads, g);
    });
    t.t_pad_rem = Gamepad::GamepadRemoved([](IInspectable const&, Gamepad const& g)
    {
        std::lock_guard<std::mutex> lock(T().mutex);
        Remove(T().pads, g);
    });
    t.t_wheel_add = RacingWheel::RacingWheelAdded([](IInspectable const&, RacingWheel const& w)
    {
        std::lock_guard<std::mutex> lock(T().mutex);
        AddUnique(T().wheels, w);
    });
    t.t_wheel_rem = RacingWheel::RacingWheelRemoved([](IInspectable const&, RacingWheel const& w)
    {
        std::lock_guard<std::mutex> lock(T().mutex);
        Remove(T().wheels, w);
    });

    // Devices that were already connected before the handlers were registered.
    std::lock_guard<std::mutex> lock(t.mutex);
    for (Gamepad const& g : Gamepad::Gamepads())         AddUnique(t.pads, g);
    for (RacingWheel const& w : RacingWheel::RacingWheels()) AddUnique(t.wheels, w);
}

void Shutdown()
{
    Tracker& t = T();
    if (!t.started)
        return;
    Gamepad::GamepadAdded(t.t_pad_add);
    Gamepad::GamepadRemoved(t.t_pad_rem);
    RacingWheel::RacingWheelAdded(t.t_wheel_add);
    RacingWheel::RacingWheelRemoved(t.t_wheel_rem);
    std::lock_guard<std::mutex> lock(t.mutex);
    t.pads.clear();
    t.wheels.clear();
    t.started = false;
}

// =================================================================================================
// Gamepad -> OIS::JoyStick (layout identical to the Xbox 360 DirectInput driver)
// =================================================================================================

GamepadJoyStick::GamepadJoyStick(int dev_id)
    : OIS::JoyStick(GAMEPAD_VENDOR, /*buffered=*/true, dev_id, /*creator=*/nullptr)
{
}

void GamepadJoyStick::_initialize()
{
    mState.mButtons.assign(10, false);
    mState.mAxes.assign(5, OIS::Axis());
    mState.clear();
    mPOVs = 1;
    mSliders = 0;
}

void GamepadJoyStick::capture()
{
    OIS::JoyStickState next = mState;
    next.clear(); // neutral when no pad is connected

    Gamepad pad = FirstGamepad();
    if (pad)
    {
        GamepadReading r = pad.GetCurrentReading();
        const uint32_t b = static_cast<uint32_t>(r.Buttons);
        auto has = [b](GamepadButtons f) { return (b & static_cast<uint32_t>(f)) != 0; };

        next.mAxes[0].abs = -StickToAxis(r.LeftThumbstickY);   // DirectInput: up = negative
        next.mAxes[1].abs =  StickToAxis(r.LeftThumbstickX);
        next.mAxes[2].abs = -StickToAxis(r.RightThumbstickY);
        next.mAxes[3].abs =  StickToAxis(r.RightThumbstickX);
        next.mAxes[4].abs =  StickToAxis(r.LeftTrigger - r.RightTrigger); // shared Z axis: LT +, RT -

        next.mButtons[0] = has(GamepadButtons::A);
        next.mButtons[1] = has(GamepadButtons::B);
        next.mButtons[2] = has(GamepadButtons::X);
        next.mButtons[3] = has(GamepadButtons::Y);
        next.mButtons[4] = has(GamepadButtons::LeftShoulder);
        next.mButtons[5] = has(GamepadButtons::RightShoulder);
        next.mButtons[6] = has(GamepadButtons::View);
        next.mButtons[7] = has(GamepadButtons::Menu);
        next.mButtons[8] = has(GamepadButtons::LeftThumbstick);
        next.mButtons[9] = has(GamepadButtons::RightThumbstick);

        next.mPOV[0].direction = PovFromBits(has(GamepadButtons::DPadUp), has(GamepadButtons::DPadDown),
                                             has(GamepadButtons::DPadLeft), has(GamepadButtons::DPadRight));
    }
    CommitAndNotify(this, mState, next, mListener, mBuffered);
}

// =================================================================================================
// RacingWheel -> OIS::JoyStick (+ force feedback)
// =================================================================================================

static const int WHEEL_BUTTON_BITS = 22; // RacingWheelButtons: PreviousGear..Button16
static const int WHEEL_GEAR_FIRST  = WHEEL_BUTTON_BITS; // buttons 22..29 = gears 1..8
static const int WHEEL_GEAR_COUNT  = 8;
static const int WHEEL_GEAR_REV    = WHEEL_GEAR_FIRST + WHEEL_GEAR_COUNT; // 30

RacingWheelJoyStick::RacingWheelJoyStick(int dev_id)
    : OIS::JoyStick(WHEEL_VENDOR, /*buffered=*/true, dev_id, /*creator=*/nullptr)
{
}

RacingWheelJoyStick::~RacingWheelJoyStick() = default;

void RacingWheelJoyStick::_initialize()
{
    mState.mButtons.assign(WHEEL_GEAR_REV + 1, false);
    mState.mAxes.assign(5, OIS::Axis());
    mState.clear();
    mPOVs = 0;
    mSliders = 0;
}

void RacingWheelJoyStick::capture()
{
    OIS::JoyStickState next = mState;
    next.clear();
    // Pedals rest at 'released'.
    next.mAxes[1].abs = next.mAxes[2].abs = next.mAxes[3].abs = next.mAxes[4].abs = PedalToAxis(0.0);

    RacingWheel wheel = FirstWheel();
    if (wheel)
    {
        RacingWheelReading r = wheel.GetCurrentReading();
        next.mAxes[0].abs = StickToAxis(r.Wheel);
        next.mAxes[1].abs = PedalToAxis(r.Brake);
        next.mAxes[2].abs = PedalToAxis(r.Throttle);
        next.mAxes[3].abs = PedalToAxis(wheel.HasClutch() ? r.Clutch : 0.0);
        next.mAxes[4].abs = PedalToAxis(wheel.HasHandbrake() ? r.Handbrake : 0.0);

        const uint32_t bits = static_cast<uint32_t>(r.Buttons);
        for (int i = 0; i < WHEEL_BUTTON_BITS; ++i)
            next.mButtons[i] = (bits & (1u << i)) != 0;

        if (wheel.HasPatternShifter())
        {
            const int gear = r.PatternShifterGear; // 0 = neutral, -1 = reverse
            if (gear >= 1 && gear <= WHEEL_GEAR_COUNT)
                next.mButtons[WHEEL_GEAR_FIRST + gear - 1] = true;
            else if (gear < 0)
                next.mButtons[WHEEL_GEAR_REV] = true;
        }
    }
    CommitAndNotify(this, mState, next, mListener, mBuffered);
}

OIS::Interface* RacingWheelJoyStick::queryInterface(OIS::Interface::IType type)
{
    if (type != OIS::Interface::ForceFeedback)
        return nullptr;
    RacingWheel wheel = FirstWheel();
    if (!wheel || !wheel.WheelMotor())
        return nullptr;
    if (!m_ffb)
        m_ffb.reset(new WheelForceFeedback());
    return m_ffb.get();
}

// -------------------------------------------------------------------------------------------------

struct WheelForceFeedback::Impl
{
    ForceFeedbackMotor                motor{ nullptr };
    ConstantForceEffect               effect{ nullptr };
    std::shared_ptr<std::atomic<int>> state = std::make_shared<std::atomic<int>>(0); // 0 none, 1 loading, 2 ready, -1 failed
    bool                              playing = false;
    double                            gain = 1.0;

    bool EnsureMotor()
    {
        if (motor) return true;
        RacingWheel wheel = FirstWheel();
        if (!wheel) return false;
        motor = wheel.WheelMotor();
        return (bool)motor;
    }
};

WheelForceFeedback::WheelForceFeedback() : m(new Impl())
{
    _addEffectTypes(OIS::Effect::ConstantForce, OIS::Effect::Constant);
}

WheelForceFeedback::~WheelForceFeedback()
{
    try
    {
        if (m->effect && m->playing) m->effect.Stop();
        if (m->motor && m->effect)   m->motor.TryUnloadEffectAsync(m->effect); // fire-and-forget
    }
    catch (...) {}
}

void WheelForceFeedback::setMasterGain(float level)
{
    m->gain = std::max(0.f, std::min(1.f, level));
    if (m->EnsureMotor())
        m->motor.MasterGain(m->gain);
}

void WheelForceFeedback::setAutoCenterMode(bool /*auto_on*/)
{
    // RoR computes its own centering force (io_ffb_center_gain) -> nothing to do.
}

void WheelForceFeedback::upload(const OIS::Effect* /*effect*/)
{
    if (!m->EnsureMotor() || m->state->load() != 0)
        return;
    m->effect = ConstantForceEffect();
    m->state->store(1);
    std::shared_ptr<std::atomic<int>> st = m->state;
    // Asynchronous: blocking with .get() on the CoreWindow thread throws in UWP (STA).
    m->motor.LoadEffectAsync(m->effect).Completed(
        [st](IAsyncOperation<ForceFeedbackLoadEffectResult> const& op, AsyncStatus status)
        {
            const bool ok = status == AsyncStatus::Completed &&
                            op.GetResults() == ForceFeedbackLoadEffectResult::Succeeded;
            st->store(ok ? 2 : -1);
        });
}

void WheelForceFeedback::modify(const OIS::Effect* effect)
{
    if (!effect || m->state->load() != 2)
        return;
    const OIS::ConstantEffect* c = dynamic_cast<const OIS::ConstantEffect*>(effect->getForceEffect());
    if (!c)
        return;
    // OIS level is -10000..10000 ; WGI expects a direction vector with magnitude 0..1 on the X axis.
    // If the force feels inverted on a given wheel, flip the sign here.
    const float magnitude = std::max(-1.f, std::min(1.f, c->level / 10000.f));
    const TimeSpan duration = std::chrono::duration_cast<TimeSpan>(std::chrono::hours(1)); // ~OIS_INFINITE
    m->effect.SetParameters(winrt::Windows::Foundation::Numerics::float3(magnitude, 0.f, 0.f), duration);
    if (!m->playing)
    {
        m->effect.Start();
        m->playing = true;
    }
}

void WheelForceFeedback::remove(const OIS::Effect* /*effect*/)
{
    if (m->effect && m->playing)
    {
        m->effect.Stop();
        m->playing = false;
    }
}

// =================================================================================================
// CoreWindow keyboard -> OIS::Keyboard. OIS key codes ARE DirectInput codes = PS/2 set-1 scancodes
// with bit 0x80 for extended (E0-prefixed) keys, so the mapping is arithmetic, not a table.
// =================================================================================================

CoreWindowKeyboard::CoreWindowKeyboard(int dev_id)
    : OIS::Keyboard("CoreWindow Keyboard", /*buffered=*/true, dev_id, /*creator=*/nullptr)
{
}

CoreWindowKeyboard::~CoreWindowKeyboard()
{
    Uwp::SetKeyboardHandler(nullptr);
    Uwp::SetCharacterHandler(nullptr);
}

void CoreWindowKeyboard::_initialize()
{
    Uwp::SetKeyboardHandler([this](Uwp::KeyboardEvent const& e)
    {
        const unsigned code = (e.scancode & 0x7F) | (e.extended ? 0x80u : 0u);
        m_queue.push_back({ static_cast<OIS::KeyCode>(code), 0u, e.down });
    });
    Uwp::SetCharacterHandler([this](uint32_t cp)
    {
        if (cp < 0x20 && cp != '\t' && cp != '\r') return; // control chars come as key codes
        // OIS delivers text together with keyPressed: attach to the pending key-down if possible.
        for (auto it = m_queue.rbegin(); it != m_queue.rend(); ++it)
        {
            if (it->down && it->text == 0) { it->text = cp; return; }
        }
        m_queue.push_back({ OIS::KC_UNASSIGNED, cp, true });
        m_queue.push_back({ OIS::KC_UNASSIGNED, 0u, false });
    });
}

void CoreWindowKeyboard::capture()
{
    while (!m_queue.empty())
    {
        Ev ev = m_queue.front();
        m_queue.pop_front();
        const unsigned idx = static_cast<unsigned>(ev.kc) & 0xFF;
        if (ev.kc != OIS::KC_UNASSIGNED)
            m_keys[idx] = ev.down;

        unsigned int mod = 0;
        if (ev.kc == OIS::KC_LSHIFT   || ev.kc == OIS::KC_RSHIFT)   mod = Shift;
        if (ev.kc == OIS::KC_LCONTROL || ev.kc == OIS::KC_RCONTROL) mod = Ctrl;
        if (ev.kc == OIS::KC_LMENU    || ev.kc == OIS::KC_RMENU)    mod = Alt;
        if (mod) { if (ev.down) mModifiers |= mod; else mModifiers &= ~mod; }

        if (mBuffered && mListener)
        {
            OIS::KeyEvent ke(this, ev.kc, ev.text);
            if (ev.down) mListener->keyPressed(ke);
            else         mListener->keyReleased(ke);
        }
    }
}

bool CoreWindowKeyboard::isKeyDown(OIS::KeyCode key) const
{
    return m_keys[static_cast<unsigned>(key) & 0xFF];
}

const std::string& CoreWindowKeyboard::getAsString(OIS::KeyCode kc)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "Key_%02X", static_cast<unsigned>(kc) & 0xFF);
    m_name_buf = buf;
    return m_name_buf;
}

OIS::KeyCode CoreWindowKeyboard::getAsKeyCode(std::string str)
{
    unsigned v = 0;
    if (std::sscanf(str.c_str(), "Key_%02X", &v) == 1)
        return static_cast<OIS::KeyCode>(v & 0xFF);
    return OIS::KC_UNASSIGNED;
}

void CoreWindowKeyboard::copyKeyStates(char keys[256]) const
{
    for (int i = 0; i < 256; ++i)
        keys[i] = m_keys[i] ? 1 : 0;
}

// =================================================================================================
// CoreWindow pointer/mouse -> OIS::Mouse
// =================================================================================================

CoreWindowMouse::CoreWindowMouse(int dev_id)
    : OIS::Mouse("CoreWindow Mouse", /*buffered=*/true, dev_id, /*creator=*/nullptr)
{
}

CoreWindowMouse::~CoreWindowMouse()
{
    Uwp::SetPointerHandler(nullptr);
    Uwp::SetMouseDeltaHandler(nullptr);
}

void CoreWindowMouse::_initialize()
{
    int w = 0, h = 0;
    Uwp::GetWindowPixelSize(w, h);
    m_abs_x = w * 0.5f;
    m_abs_y = h * 0.5f;
    Uwp::SetPointerHandler([this](Uwp::PointerEvent const& e)
    {
        m_abs_x = e.x;
        m_abs_y = e.y;
        m_buttons = (e.left ? 1 << OIS::MB_Left : 0) | (e.right ? 1 << OIS::MB_Right : 0) |
                    (e.middle ? 1 << OIS::MB_Middle : 0);
        m_rel_z += e.wheel_delta;
        m_moved = true;
    });
    Uwp::SetMouseDeltaHandler([this](int dx, int dy)
    {
        m_rel_x += dx;
        m_rel_y += dy;
        m_moved = true;
    });
}

void CoreWindowMouse::capture()
{
    // Keep the clamping area in sync with the CoreWindow (OIS defaults to 50x50 until told otherwise).
    int win_w = 0, win_h = 0;
    Uwp::GetWindowPixelSize(win_w, win_h);
    if (win_w > 0 && win_h > 0)
    {
        mState.width = win_w;
        mState.height = win_h;
    }
    const int prev_x = mState.X.abs, prev_y = mState.Y.abs;
    const int max_x = std::max(0, mState.width - 1), max_y = std::max(0, mState.height - 1);
    mState.X.abs = std::max(0, std::min(max_x, (int)m_abs_x));
    mState.Y.abs = std::max(0, std::min(max_y, (int)m_abs_y));
    mState.X.rel = m_rel_x ? m_rel_x : (mState.X.abs - prev_x);
    mState.Y.rel = m_rel_y ? m_rel_y : (mState.Y.abs - prev_y);
    mState.Z.rel = m_rel_z;
    mState.Z.abs += m_rel_z;
    mState.buttons = m_buttons;

    const bool moved = m_moved && (mState.X.rel || mState.Y.rel || mState.Z.rel);
    m_rel_x = m_rel_y = m_rel_z = 0;
    m_moved = false;

    if (!mBuffered || !mListener)
    {
        m_prev_buttons = m_buttons;
        return;
    }
    OIS::MouseEvent ev(this, mState);
    if (moved)
        mListener->mouseMoved(ev);
    for (int b = OIS::MB_Left; b <= OIS::MB_Middle; ++b)
    {
        const int bit = 1 << b;
        if ((m_buttons & bit) != (m_prev_buttons & bit))
        {
            if (m_buttons & bit) mListener->mousePressed(ev, static_cast<OIS::MouseButtonID>(b));
            else                 mListener->mouseReleased(ev, static_cast<OIS::MouseButtonID>(b));
        }
    }
    m_prev_buttons = m_buttons;
}

// =================================================================================================
// Dear ImGui 1.73 gamepad navigation
// =================================================================================================

void FeedImGuiGamepadNav(ImGuiIO& io)
{
    for (float& v : io.NavInputs) v = 0.f;
    if (!(io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad))
        return;
    Gamepad pad = FirstGamepad();
    if (!pad)
    {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    GamepadReading r = pad.GetCurrentReading();
    const uint32_t b = static_cast<uint32_t>(r.Buttons);
    auto btn = [b](GamepadButtons f) { return (b & static_cast<uint32_t>(f)) ? 1.f : 0.f; };
    auto pos = [](double v) { return v > 0.25 ? (float)std::min(1.0, (v - 0.25) / 0.75) : 0.f; };

    io.NavInputs[ImGuiNavInput_Activate]    = btn(GamepadButtons::A);
    io.NavInputs[ImGuiNavInput_Cancel]      = btn(GamepadButtons::B);
    io.NavInputs[ImGuiNavInput_Input]       = btn(GamepadButtons::Y);
    io.NavInputs[ImGuiNavInput_Menu]        = btn(GamepadButtons::X);
    io.NavInputs[ImGuiNavInput_DpadLeft]    = btn(GamepadButtons::DPadLeft);
    io.NavInputs[ImGuiNavInput_DpadRight]   = btn(GamepadButtons::DPadRight);
    io.NavInputs[ImGuiNavInput_DpadUp]      = btn(GamepadButtons::DPadUp);
    io.NavInputs[ImGuiNavInput_DpadDown]    = btn(GamepadButtons::DPadDown);
    io.NavInputs[ImGuiNavInput_FocusPrev]   = btn(GamepadButtons::LeftShoulder);
    io.NavInputs[ImGuiNavInput_FocusNext]   = btn(GamepadButtons::RightShoulder);
    io.NavInputs[ImGuiNavInput_TweakSlow]   = btn(GamepadButtons::LeftShoulder);
    io.NavInputs[ImGuiNavInput_TweakFast]   = btn(GamepadButtons::RightShoulder);
    io.NavInputs[ImGuiNavInput_LStickLeft]  = pos(-r.LeftThumbstickX);
    io.NavInputs[ImGuiNavInput_LStickRight] = pos(r.LeftThumbstickX);
    io.NavInputs[ImGuiNavInput_LStickUp]    = pos(r.LeftThumbstickY);
    io.NavInputs[ImGuiNavInput_LStickDown]  = pos(-r.LeftThumbstickY);
}

} // namespace XboxInput
} // namespace RoR
