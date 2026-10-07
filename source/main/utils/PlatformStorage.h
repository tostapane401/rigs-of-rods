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

/// @file   PlatformStorage.h
/// @brief  Sandbox-aware storage roots. The ONLY place that decides where RoR may write.
///
///  Backend              user_dir (config, mods, saves)       cache_dir (modcache, shaders)     install_dir
///  -------------------  -----------------------------------  --------------------------------  -----------------
///  Win32 (desktop)      Documents\My Games\Rigs of Rods      <user_dir>\cache                  exe folder (rw)
///  Win32 portable       <exe>\config                         <user_dir>\cache                  exe folder (rw)
///  UWP / Xbox DevMode   ApplicationData.LocalFolder\RoR      ApplicationData.LocalCacheFolder  Package InstalledLocation (READ-ONLY)
///  GDK (PC/console)     XPersistentLocalStorage path\RoR     <PLS>\cache                       XGameGetInstallLocation (READ-ONLY)
///  POSIX                $HOME/.rigsofrods                    <user_dir>/cache                  exe folder
///
/// Rules enforced by the callers:
///  * Never write under install_dir when install_dir_readonly == true (UWP/GDK packages are
///    mounted read-only; writing there is an access violation / E_ACCESSDENIED).
///  * Never build paths from %USERPROFILE%, SHGetFolderPath, getenv() on UWP/GDK.
///  * All paths are UTF-8 and use the platform separator (RoR::PATH_SLASH).

#pragma once

#include <string>

namespace RoR {
namespace PlatformStorage {

enum class Backend
{
    WIN32_DOCUMENTS,
    WIN32_PORTABLE,
    UWP_APPDATA,
    GDK_PERSISTENT_LOCAL,
    POSIX_HOME,
};

struct Paths
{
    Backend     backend = Backend::POSIX_HOME;
    std::string install_dir;          //!< Where the executable + 'resources' live.
    bool        install_dir_readonly = false;
    std::string user_dir;             //!< Root for config/, mods/, savegames/, logs/ ... (RoR 'sys_user_dir').
    std::string cache_dir;            //!< Regenerable data, may be purged by the OS (modcache, shaders).
    std::string temp_dir;             //!< Scratch space, may disappear between launches.
};

/// Resolves storage roots for the current platform. Creates the folders. Never throws.
/// @return false on failure; 'error' then holds a human-readable reason.
bool Resolve(Paths& out, std::string& error);

/// Paths resolved by the last successful Resolve() call.
const Paths& Get();

const char* BackendName(Backend b);

} // namespace PlatformStorage
} // namespace RoR
