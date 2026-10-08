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

/// @file   XboxInput.h
/// @brief  Native Xbox input as drop-in OIS devices.
///
/// Strategy: keep OIS *types* (KeyCode, JoyStickState, JoyStickListener, ForceFeedback) because
/// InputEngine, input.map files, the controls UI and ForceFeedback.cpp are written against them,
/// and replace only the OIS *backends* (DirectInput/Win32, which do not exist in the UWP API set)
/// with implementations on top of:
///   - Windows.Gaming.Input  (Gamepad, RacingWheel + WheelMotor force feedback)  -> UWP / Xbox
///   - CoreWindow events      (USB keyboard / mouse on Xbox)                      -> UWP / Xbox
/// A GameInput (GDK) backend can implement the same three classes later; nothing else changes.
///
/// Device layout is chosen so that EXISTING RoR map files keep working:
///   joystick 0 = "Controller (Xbox 360 Wireless Receiver for Windows)"  -> Controller__Xbox_360_Wireless_Receiver_for_Windows_.map
///                 axes 0=LS-Y(inv) 1=LS-X 2=RS-Y(inv) 3=RS-X 4=LT-RT, buttons A B X Y LB RB View Menu LS RS, POV 0 = D-pad
///   joystick 1 = "Xbox Racing Wheel (Windows.Gaming.Input)"            -> Xbox_Racing_Wheel__Windows_Gaming_Input_.map
///                 axes 0=wheel 1=brake 2=throttle 3=clutch 4=handbrake (pedals G27-style: released=+MAX, pressed=-MAX)
///                 buttons 0..21 = RacingWheelButtons bits, 22..29 = H-shifter gears 1..8, 30 = reverse

#pragma once

#include <OISForceFeedback.h>
#include <OISJoyStick.h>
#include <OISKeyboard.h>
#include <OISMouse.h>

#include <deque>
#include <memory>
#include <string>

struct ImGuiIO;

namespace RoR {
namespace XboxInput {

constexpr const char* GAMEPAD_VENDOR = "Controller (Xbox 360 Wireless Receiver for Windows)";
constexpr const char* WHEEL_VENDOR   = "Xbox Racing Wheel (Windows.Gaming.Input)";

/// Starts Windows.Gaming.Input device tracking (hot-plug safe). Call once, before creating devices.
void Startup();
void Shutdown();

// -------------------------------------------------------------------------------------------------

class GamepadJoyStick : public OIS::JoyStick
{
public:
    explicit GamepadJoyStick(int dev_id);
    void setBuffered(bool buffered) override { mBuffered = buffered; }
    void capture() override;
    OIS::Interface* queryInterface(OIS::Interface::IType) override { return nullptr; }
    void _initialize() override;
};

class WheelForceFeedback;

class RacingWheelJoyStick : public OIS::JoyStick
{
public:
    explicit RacingWheelJoyStick(int dev_id);
    ~RacingWheelJoyStick() override;
    void setBuffered(bool buffered) override { mBuffered = buffered; }
    void capture() override;
    OIS::Interface* queryInterface(OIS::Interface::IType type) override;
    void _initialize() override;
private:
    std::unique_ptr<WheelForceFeedback> m_ffb;
};

/// OIS::ForceFeedback on top of RacingWheel.WheelMotor (ConstantForceEffect). Non-blocking:
/// effect loading is asynchronous (blocking .get() on the UI thread is forbidden in UWP).
class WheelForceFeedback : public OIS::ForceFeedback
{
public:
    WheelForceFeedback();
    ~WheelForceFeedback() override;
    void setMasterGain(float level) override;
    void setAutoCenterMode(bool auto_on) override;
    void upload(const OIS::Effect* effect) override;
    void modify(const OIS::Effect* effect) override;
    void remove(const OIS::Effect* effect) override;
    short getFFAxesNumber() override { return 1; }
    unsigned short getFFMemoryLoad() override { return 0; }
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// -------------------------------------------------------------------------------------------------

class CoreWindowKeyboard : public OIS::Keyboard
{
public:
    explicit CoreWindowKeyboard(int dev_id);
    ~CoreWindowKeyboard() override;
    void setBuffered(bool buffered) override { mBuffered = buffered; }
    void capture() override;
    OIS::Interface* queryInterface(OIS::Interface::IType) override { return nullptr; }
    void _initialize() override;
    bool isKeyDown(OIS::KeyCode key) const override;
    const std::string& getAsString(OIS::KeyCode kc) override;
    OIS::KeyCode getAsKeyCode(std::string str) override;
    void copyKeyStates(char keys[256]) const override;
private:
    struct Ev { OIS::KeyCode kc; unsigned int text; bool down; };
    std::deque<Ev> m_queue;
    bool           m_keys[256] = {};
    std::string    m_name_buf;
};

class CoreWindowMouse : public OIS::Mouse
{
public:
    explicit CoreWindowMouse(int dev_id);
    ~CoreWindowMouse() override;
    void setBuffered(bool buffered) override { mBuffered = buffered; }
    void capture() override;
    OIS::Interface* queryInterface(OIS::Interface::IType) override { return nullptr; }
    void _initialize() override;
private:
    float    m_abs_x = 0.f, m_abs_y = 0.f;
    int      m_rel_x = 0, m_rel_y = 0, m_rel_z = 0;
    int      m_buttons = 0, m_prev_buttons = 0;
    bool     m_moved = false;
    int      m_pad_buttons = 0;                 // virtual cursor (controller A/B)
    float    m_scroll_acc = 0.f;
    long long m_last_tick_us = 0;
};

// -------------------------------------------------------------------------------------------------

/// Controller as mouse for RoR's PC-style menus ("virtual cursor"): left stick moves the
/// pointer, A = left click, B = right click, right stick = mouse wheel. Drives CoreWindowMouse,
/// so ImGui, MyGUI and the scene see ordinary mouse events. Enable it while a menu is open.
void SetVirtualCursorEnabled(bool on);
bool IsVirtualCursorEnabled();

/// Controller D-pad -> arrow keys, X -> Enter, for RoR's keyboard-driven panels (main menu,
/// selectors). OR-ed with the real keyboard. Call every frame before ImGui::NewFrame().
void FeedImGuiGamepadKeys(ImGuiIO& io, const OIS::Keyboard* kb);

/// One line of live input state (devices, buttons, sticks, event counters) for bring-up.
std::string DebugStatus();

/// Feeds Dear ImGui 1.73 gamepad navigation (io.NavInputs) from the first gamepad.
/// Call every frame before ImGui::NewFrame(). Requires ImGuiConfigFlags_NavEnableGamepad.
void FeedImGuiGamepadNav(ImGuiIO& io);

} // namespace XboxInput
} // namespace RoR
