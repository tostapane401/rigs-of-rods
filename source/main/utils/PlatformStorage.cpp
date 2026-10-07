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

#include "PlatformStorage.h"

#include "PlatformUtils.h"

#if defined(ROR_PLATFORM_UWP)
#   include <winrt/base.h>
#   include <winrt/Windows.ApplicationModel.h>
#   include <winrt/Windows.Storage.h>
#elif defined(ROR_PLATFORM_GDK)
#   include <Windows.h>
#   include <XGameRuntimeInit.h>
#   include <XPersistentLocalStorage.h>
#endif

#include <cstdlib>
#include <vector>

namespace RoR {
namespace PlatformStorage {

static Paths g_paths;

const Paths& Get() { return g_paths; }

const char* BackendName(Backend b)
{
    switch (b)
    {
    case Backend::WIN32_DOCUMENTS:      return "Win32 (Documents\\My Games)";
    case Backend::WIN32_PORTABLE:       return "Win32 (portable)";
    case Backend::UWP_APPDATA:          return "UWP (ApplicationData)";
    case Backend::GDK_PERSISTENT_LOCAL: return "GDK (PersistentLocalStorage)";
    case Backend::POSIX_HOME:           return "POSIX ($HOME)";
    }
    return "?";
}

static bool EnsureFolder(std::string const& path, std::string& error)
{
    if (path.empty())
    {
        error = "empty path";
        return false;
    }
    CreateFolder(path);
    if (!FolderExists(path))
    {
        error = "cannot create folder '" + path + "'";
        return false;
    }
    return true;
}

#if defined(ROR_PLATFORM_UWP)

// -------------------------------------------------------------------------------------------------
// UWP (Xbox Dev Mode, Windows Store). Everything outside these folders is denied by the AppContainer.
// Win32 file APIs (CreateFile2, std::fstream, _wfopen ...) ARE allowed inside them, which is what
// OGRE's FileSystemArchive and RoR's own code use - no StorageFile async plumbing needed.
// -------------------------------------------------------------------------------------------------
static bool ResolveImpl(Paths& out, std::string& error)
{
    using namespace winrt::Windows;
    try
    {
        Storage::ApplicationData data = Storage::ApplicationData::Current();
        out.backend              = Backend::UWP_APPDATA;
        out.install_dir          = winrt::to_string(ApplicationModel::Package::Current().InstalledLocation().Path());
        out.install_dir_readonly = true;
        // LocalFolder: persistent, backed up with the console profile data; NOT roamed.
        out.user_dir  = PathCombine(winrt::to_string(data.LocalFolder().Path()), "RigsOfRods");
        // LocalCacheFolder: persistent but excluded from backup -> ideal for modcache/shaders.
        out.cache_dir = winrt::to_string(data.LocalCacheFolder().Path());
        out.temp_dir  = winrt::to_string(data.TemporaryFolder().Path());
    }
    catch (winrt::hresult_error const& e)
    {
        error = "ApplicationData unavailable: " + winrt::to_string(e.message());
        return false;
    }
    return true;
}

#elif defined(ROR_PLATFORM_GDK)

// -------------------------------------------------------------------------------------------------
// Microsoft GDK. Requires XGameRuntimeInitialize() (done here if needed) and, on console,
// <PersistentLocalStorage><SizeMB>...</SizeMB></PersistentLocalStorage> in MicrosoftGame.config.
// For cloud-synced saves use XGameSaveFiles (XGameSaveFilesGetFolderWithUiAsync) for 'savegames'.
// -------------------------------------------------------------------------------------------------
static std::string GdkExeDir()
{
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
    buf.resize(len);
    int n = WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string exe(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, &exe[0], n, nullptr, nullptr);
    return GetParentDirectory(exe.c_str());
}

static bool ResolveImpl(Paths& out, std::string& error)
{
    HRESULT hr = XGameRuntimeInitialize();
    if (FAILED(hr)) // reference-counted: repeated calls return S_OK
    {
        error = "XGameRuntimeInitialize failed";
        return false;
    }
    size_t size = 0;
    if (FAILED(XPersistentLocalStorageGetPathSize(&size)) || size == 0)
    {
        error = "PersistentLocalStorage not configured in MicrosoftGame.config";
        return false;
    }
    std::vector<char> path(size, '\0');
    if (FAILED(XPersistentLocalStorageGetPath(size, path.data(), nullptr)))
    {
        error = "XPersistentLocalStorageGetPath failed";
        return false;
    }
    std::string pls(path.data());
    if (!pls.empty() && (pls.back() == '\\' || pls.back() == '/'))
        pls.pop_back();

    out.backend              = Backend::GDK_PERSISTENT_LOCAL;
    out.install_dir          = GdkExeDir();
    out.install_dir_readonly = true;
    out.user_dir             = PathCombine(pls, "RigsOfRods");
    out.cache_dir            = PathCombine(pls, "cache");
    out.temp_dir             = PathCombine(pls, "temp");
    return true;
}

#else

// -------------------------------------------------------------------------------------------------
// Desktop: unchanged behaviour (portable 'config' folder next to the exe, else Documents\My Games).
// -------------------------------------------------------------------------------------------------
static bool ResolveImpl(Paths& out, std::string& error)
{
    const std::string exe_path = GetExecutablePath();
    if (exe_path.empty())
    {
        error = "cannot retrieve executable path";
        return false;
    }
    out.install_dir          = GetParentDirectory(exe_path.c_str());
    out.install_dir_readonly = false;

    const std::string portable_dir = PathCombine(out.install_dir, "config");
    if (FolderExists(portable_dir))
    {
        out.backend  = Backend::WIN32_PORTABLE;
        out.user_dir = portable_dir;
    }
    else
    {
        const std::string home = GetUserHomeDirectory();
        if (home.empty())
        {
            error = "cannot retrieve user home directory";
            return false;
        }
#if defined(_WIN32)
        out.backend = Backend::WIN32_DOCUMENTS;
        const std::string my_games = PathCombine(home, "My Games");
        CreateFolder(my_games);
        out.user_dir = PathCombine(my_games, "Rigs of Rods");
#else
        out.backend = Backend::POSIX_HOME;
        const char* snap = getenv("SNAP_USER_COMMON");
#   if defined(__APPLE__)
        out.user_dir = snap ? std::string(snap) : PathCombine(home, "RigsOfRods");
#   else
        out.user_dir = snap ? std::string(snap) : PathCombine(home, ".rigsofrods");
#   endif
#endif
    }
    out.cache_dir = PathCombine(out.user_dir, "cache");
    out.temp_dir  = PathCombine(out.user_dir, "temp");
    return true;
}

#endif

bool Resolve(Paths& out, std::string& error)
{
    out = Paths();
    if (!ResolveImpl(out, error))
        return false;
    if (!EnsureFolder(out.user_dir, error) ||
        !EnsureFolder(out.cache_dir, error) ||
        !EnsureFolder(out.temp_dir, error))
    {
        return false;
    }
    g_paths = out;
    return true;
}

} // namespace PlatformStorage
} // namespace RoR
