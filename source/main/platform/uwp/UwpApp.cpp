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

// C++/WinRT (no /ZW needed in RoR code). Built only when ROR_PLATFORM_UWP is defined.

#include "UwpApp.h"

#include "AppContext.h"
#include "Application.h"
#include "Console.h"
#include "RTShaderBootstrap.h"

#include <d3d11.h>
#include <dxgi1_3.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Devices::Input;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::ViewManagement;

namespace {

CoreWindow g_window{ nullptr };
bool       g_window_closed = false;
bool       g_visible       = true;

std::function<void(RoR::Uwp::KeyboardEvent const&)> g_on_key;
std::function<void(uint32_t)>                       g_on_char;
std::function<void(RoR::Uwp::PointerEvent const&)>  g_on_pointer;
std::function<void(int, int)>                       g_on_mouse_delta;

float PixelScale()
{
    try { return (float)DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel(); }
    catch (...) { return 1.f; }
}

void DispatchPointer(PointerEventArgs const& args, int wheel)
{
    if (!g_on_pointer)
        return;
    auto pt = args.CurrentPoint();
    auto props = pt.Properties();
    const float s = PixelScale();
    RoR::Uwp::PointerEvent ev;
    ev.x = pt.Position().X * s;
    ev.y = pt.Position().Y * s;
    ev.left   = props.IsLeftButtonPressed();
    ev.right  = props.IsRightButtonPressed();
    ev.middle = props.IsMiddleButtonPressed();
    ev.wheel_delta = wheel;
    g_on_pointer(ev);
}

/// Process Lifetime Management. Xbox gives a short deadline to suspend; keep this fast.
void OnSuspendingImpl()
{
    using namespace RoR;
    try
    {
        if (App::GetConsole())
            App::GetConsole()->saveConfig();           // RoR.cfg -> LocalFolder
        RTShaderBootstrap::Get().SaveMicrocodeCache(); // LocalCacheFolder

        // Required by the UWP/Xbox certification rules for D3D11 apps: release driver
        // scratch memory on suspend (IDXGIDevice3::Trim).
        Ogre::RenderWindow* rw = App::GetAppContext() ? App::GetAppContext()->GetRenderWindow() : nullptr;
        if (rw)
        {
            ID3D11Device* device = nullptr;
            rw->getCustomAttribute("D3DDEVICE", &device);
            if (device)
            {
                com_ptr<ID3D11DeviceContext> ctx;
                device->GetImmediateContext(ctx.put());
                if (ctx)
                    ctx->ClearState();
                com_ptr<IDXGIDevice3> dxgi;
                if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice3), dxgi.put_void())))
                    dxgi->Trim();
            }
        }
    }
    catch (...)
    {
        // Never let an exception escape a PLM handler: the OS would terminate the app.
    }
}

struct RoRFrameworkView : implements<RoRFrameworkView, IFrameworkViewSource, IFrameworkView> // ViewSource first: make<>() returns the first interface
{
    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const& view)
    {
        view.Activated({ this, &RoRFrameworkView::OnActivated });
        CoreApplication::Suspending({ this, &RoRFrameworkView::OnSuspending });
        CoreApplication::Resuming({ this, &RoRFrameworkView::OnResuming });
    }

    void SetWindow(CoreWindow const& window)
    {
        g_window = window;

        window.Closed([](auto&&, auto&&) { g_window_closed = true; });
        window.VisibilityChanged([](auto&&, VisibilityChangedEventArgs const& a) { g_visible = a.Visible(); });

        window.KeyDown([](auto&&, KeyEventArgs const& a)
        {
            auto st = a.KeyStatus();
            // Gamepad buttons are also reported as VirtualKey::Gamepad* with scancode 0: ignore them
            // here, the gamepad is read through Windows.Gaming.Input.
            if (st.ScanCode == 0 || !g_on_key) return;
            g_on_key({ (uint16_t)st.ScanCode, (bool)st.IsExtendedKey, true });
            a.Handled(true);
        });
        window.KeyUp([](auto&&, KeyEventArgs const& a)
        {
            auto st = a.KeyStatus();
            if (st.ScanCode == 0 || !g_on_key) return;
            g_on_key({ (uint16_t)st.ScanCode, (bool)st.IsExtendedKey, false });
            a.Handled(true);
        });
        window.CharacterReceived([](auto&&, CharacterReceivedEventArgs const& a)
        {
            if (g_on_char) g_on_char(a.KeyCode());
        });
        window.PointerMoved([](auto&&, PointerEventArgs const& a)    { DispatchPointer(a, 0); });
        window.PointerPressed([](auto&&, PointerEventArgs const& a)  { DispatchPointer(a, 0); });
        window.PointerReleased([](auto&&, PointerEventArgs const& a) { DispatchPointer(a, 0); });
        window.PointerWheelChanged([](auto&&, PointerEventArgs const& a)
        {
            DispatchPointer(a, a.CurrentPoint().Properties().MouseWheelDelta());
        });

        // Relative mouse movement (camera look with a USB mouse).
        MouseDevice::GetForCurrentView().MouseMoved([](auto&&, MouseEventArgs const& a)
        {
            if (g_on_mouse_delta) g_on_mouse_delta(a.MouseDelta().X, a.MouseDelta().Y);
        });

        // Xbox: render edge-to-edge instead of inside the TV safe area (the game UI must then
        // keep its own safe margins, ~5% per side).
        ApplicationView::GetForCurrentView().SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);

        // Xbox: the B button raises BackRequested. If not handled, the app is navigated away from.
        SystemNavigationManager::GetForCurrentView().BackRequested([](auto&&, BackRequestedEventArgs const& a)
        {
            a.Handled(true);
        });
    }

    void Load(hstring const&) {}

    void Run()
    {
        g_window.Activate();
        // Let the window become visible before OGRE creates the swap chain.
        g_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

        char arg0[] = "RoR.exe";
        char* argv[] = { arg0, nullptr };
        RoR_GameMain(1, argv); // RoR main loop; returns on shutdown.

        CoreApplication::Exit();
    }

    void Uninitialize() {}

    void OnActivated(CoreApplicationView const&, IActivatedEventArgs const&)
    {
        CoreWindow::GetForCurrentThread().Activate();
    }

    void OnSuspending(IInspectable const&, SuspendingEventArgs const& args)
    {
        // Raised on the UI thread from within CoreDispatcher::ProcessEvents(), i.e. from
        // RoR::Uwp::PumpEvents() inside the RoR main loop -> safe to touch OGRE here.
        SuspendingDeferral deferral = args.SuspendingOperation().GetDeferral();
        OnSuspendingImpl();
        deferral.Complete();
    }

    void OnResuming(IInspectable const&, IInspectable const&) {}
};

} // anonymous namespace

namespace RoR {
namespace Uwp {

size_t GetCoreWindowHandle()
{
    if (!g_window)
        g_window = CoreWindow::GetForCurrentThread();
    return reinterpret_cast<size_t>(winrt::get_abi(g_window));
}

bool PumpEvents()
{
    if (!g_window)
        return true;
    // When hidden (Guide open, other app snapped, about to suspend) block on events
    // instead of spinning the render loop.
    g_window.Dispatcher().ProcessEvents(g_visible
        ? CoreProcessEventsOption::ProcessAllIfPresent
        : CoreProcessEventsOption::ProcessOneAndAllPending);
    return !g_window_closed;
}

bool IsVisible() { return g_visible; }

void GetWindowPixelSize(int& width, int& height)
{
    width = height = 0;
    if (!g_window) return;
    const float s = PixelScale();
    auto b = g_window.Bounds();
    width  = (int)(b.Width * s + 0.5f);
    height = (int)(b.Height * s + 0.5f);
}

void SetKeyboardHandler(std::function<void(KeyboardEvent const&)> fn) { g_on_key = std::move(fn); }
void SetCharacterHandler(std::function<void(uint32_t)> fn)           { g_on_char = std::move(fn); }
void SetPointerHandler(std::function<void(PointerEvent const&)> fn)   { g_on_pointer = std::move(fn); }
void SetMouseDeltaHandler(std::function<void(int, int)> fn)           { g_on_mouse_delta = std::move(fn); }

} // namespace Uwp
} // namespace RoR

// Process entry point for the UWP package (replaces main()/WinMain()).
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment(); // MTA, as recommended for CoreApplication games
    CoreApplication::Run(make<RoRFrameworkView>());
    return 0;
}
