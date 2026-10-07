/*
    This source file is part of Rigs of Rods
    Copyright 2026 Rigs of Rods contributors

    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.
*/

/// @file
/// @brief Xbox bring-up diagnostics (UWP only): on-screen test pattern, RoR.log dumps of the
///        ImGui / Ogre overlay / MyGUI state (what could be drawing on screen).

#pragma once

#if defined(ROR_PLATFORM_UWP)

namespace Ogre { class RenderWindow; }

namespace RoR {
namespace XboxDiag {

/// Call once per frame after GUIManager::NewImGuiFrame() and before Root::renderOneFrame().
void BeforeRender(Ogre::RenderWindow* window);

/// Call once per frame right after Root::renderOneFrame().
void AfterRender(Ogre::RenderWindow* window);

} // namespace XboxDiag
} // namespace RoR

#endif
