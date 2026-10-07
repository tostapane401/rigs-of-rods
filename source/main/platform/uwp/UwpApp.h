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

/// @file   UwpApp.h
/// @brief  UWP (Xbox Dev Mode) application shell: CoreApplication/IFrameworkView,
///         CoreWindow event pump, PLM (suspend/resume) and raw CoreWindow input taps.
///         Header is WinRT-free on purpose so it can be included from any RoR TU.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

extern "C" int RoR_GameMain(int argc, char* argv[]); // main.cpp, renamed for UWP

namespace RoR {
namespace Uwp {

/// ABI pointer (ICoreWindow*) of the app's CoreWindow, as size_t, for OGRE's
/// "externalWindowHandle" render window parameter.
size_t GetCoreWindowHandle();

/// Dispatch pending CoreWindow events. Blocks while the app is not visible
/// (so a backgrounded game does not burn CPU/GPU). @return false when the window was closed.
bool PumpEvents();

bool IsVisible();

/// Size of the CoreWindow in physical pixels (DIPs * scale).
void GetWindowPixelSize(int& width, int& height);

// --- Raw input taps (CoreWindow thread == RoR main thread, no locking needed) ---

struct KeyboardEvent  { uint16_t scancode; bool extended; bool down; };
struct PointerEvent   { float x, y; bool left, right, middle; int wheel_delta; };

void SetKeyboardHandler(std::function<void(KeyboardEvent const&)> fn);
void SetCharacterHandler(std::function<void(uint32_t codepoint)> fn);
void SetPointerHandler(std::function<void(PointerEvent const&)> fn);
void SetMouseDeltaHandler(std::function<void(int dx, int dy)> fn); //!< Relative mouse (MouseDevice.MouseMoved)

} // namespace Uwp
} // namespace RoR
