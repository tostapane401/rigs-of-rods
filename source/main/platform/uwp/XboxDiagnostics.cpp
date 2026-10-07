/*
    This source file is part of Rigs of Rods
    Copyright 2026 Rigs of Rods contributors

    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.
*/

#include "XboxDiagnostics.h"

#if defined(ROR_PLATFORM_UWP)

#include "Application.h"
#include "PlatformUtils.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <OgreMaterialManager.h>
#include <OgreOverlayManager.h>
#include <OgreRenderTargetListener.h>
#include <OgreRenderWindow.h>
#include <OgreRoot.h>
#include <OgreTechnique.h>
#include <OgreTimer.h>
#include <OgreViewport.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <vector>

namespace RoR {
namespace XboxDiag {

namespace {

const unsigned long PATTERN_MS = 90000;       // on-screen test pattern lifetime
const unsigned long SHOT_MS[2] = {8000, 30000}; // screenshot + state dump times

Ogre::Timer& Clock()
{
    static Ogre::Timer t;
    return t;
}

// Writes a 24-bit bottom-up BMP, downscaled 2x (keeps the file ~1.5 MB at 1080p).
bool WriteBmpHalf(const std::string& path, const Ogre::uchar* bgra, uint32_t w, uint32_t h)
{
    const uint32_t ow = std::max(1u, w / 2), oh = std::max(1u, h / 2);
    const uint32_t row = (ow * 3 + 3) & ~3u;
    const uint32_t img = row * oh;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    std::fwrite("BM", 1, 2, f);
    u32(54 + img); u16(0); u16(0); u32(54);
    u32(40); u32(ow); u32(oh); u16(1); u16(24); u32(0); u32(img); u32(2835); u32(2835); u32(0); u32(0);
    std::vector<Ogre::uchar> line(row, 0);
    for (uint32_t y = 0; y < oh; ++y)
    {
        const Ogre::uchar* src = bgra + size_t(h - 1 - std::min(h - 1, y * 2)) * w * 4;
        for (uint32_t x = 0; x < ow; ++x)
        {
            const Ogre::uchar* p = src + size_t(std::min(w - 1, x * 2)) * 4;
            line[x * 3 + 0] = p[0];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[2];
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
    return true;
}

// Grabs the back buffer after everything is drawn, before Present.
struct ScreenshotGrabber : public Ogre::RenderTargetListener
{
    std::string path;
    bool pending = false;

    void postRenderTargetUpdate(const Ogre::RenderTargetEvent& evt) override
    {
        if (!pending)
            return;
        pending = false;
        try
        {
            Ogre::RenderTarget* rt = evt.source;
            const uint32_t w = rt->getWidth(), h = rt->getHeight();
            std::vector<Ogre::uchar> buf(size_t(w) * h * 4, 0);
            Ogre::PixelBox pb(w, h, 1, Ogre::PF_BYTE_BGRA, buf.data());
            rt->copyContentsToMemory(Ogre::Box(0, 0, w, h), pb, Ogre::RenderTarget::FB_AUTO);
            const bool ok = WriteBmpHalf(path, buf.data(), w, h);
            LogFormat("[RoR|Xbox] screenshot %ux%u -> '%s' %s", w, h, path.c_str(), ok ? "OK" : "WRITE FAILED");
        }
        catch (std::exception& e)
        {
            LogFormat("[RoR|Xbox] screenshot failed: %s", e.what());
        }
    }
};

ScreenshotGrabber g_grabber;
bool g_listener_added = false;
int g_next_shot = 0;

void DumpState(Ogre::RenderWindow* window)
{
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = g.IO;
    Ogre::OverlayManager& om = Ogre::OverlayManager::getSingleton();

    LogFormat("[RoR|Xbox] DIAG build %s %s", __DATE__, __TIME__);
    LogFormat("[RoR|Xbox] DIAG window %ux%u viewports=%d overlay-vp=%dx%d", window->getWidth(), window->getHeight(),
              (int)window->getNumViewports(), om.getViewportWidth(), om.getViewportHeight());
    for (unsigned short i = 0; i < window->getNumViewports(); ++i)
    {
        Ogre::Viewport* vp = window->getViewport(i);
        LogFormat("[RoR|Xbox] DIAG   viewport %d actual=(%d,%d %dx%d) scheme='%s' overlays=%d", i, vp->getActualLeft(),
                  vp->getActualTop(), vp->getActualWidth(), vp->getActualHeight(), vp->getMaterialScheme().c_str(),
                  (int)vp->getOverlaysEnabled());
    }

    LogFormat("[RoR|Xbox] DIAG imgui display=%.0fx%.0f fbscale=%.2fx%.2f fontsize=%.1f globalscale=%.2f mouse=(%.0f,%.0f) drawcursor=%d",
              io.DisplaySize.x, io.DisplaySize.y, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y,
              g.FontSize, io.FontGlobalScale, io.MousePos.x, io.MousePos.y, (int)io.MouseDrawCursor);
    for (ImGuiWindow* w : g.Windows)
    {
        LogFormat("[RoR|Xbox] DIAG   window '%s' pos=(%.0f,%.0f) size=(%.0f,%.0f) active=%d wasactive=%d hidden=%d",
                  w->Name, w->Pos.x, w->Pos.y, w->Size.x, w->Size.y, (int)w->Active, (int)w->WasActive, (int)w->Hidden);
    }

    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->Valid && dd->CmdListsCount > 0)
    {
        LogFormat("[RoR|Xbox] DIAG drawdata lists=%d vtx=%d displaypos=(%.0f,%.0f) displaysize=(%.0f,%.0f)",
                  dd->CmdListsCount, dd->TotalVtxCount, dd->DisplayPos.x, dd->DisplayPos.y, dd->DisplaySize.x, dd->DisplaySize.y);
        const ImDrawList* dl = dd->CmdLists[0];
        for (int i = 0; i < std::min(4, dl->VtxBuffer.Size); ++i)
        {
            const ImDrawVert& v = dl->VtxBuffer[i];
            LogFormat("[RoR|Xbox] DIAG   vtx[%d] pos=(%.1f,%.1f) uv=(%.3f,%.3f) col=%08X", i, v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col);
        }
        if (dl->CmdBuffer.Size > 0)
        {
            const ImDrawCmd& c = dl->CmdBuffer[0];
            LogFormat("[RoR|Xbox] DIAG   cmd[0] elems=%u clip=(%.0f,%.0f,%.0f,%.0f) tex=%p", c.ElemCount, c.ClipRect.x,
                      c.ClipRect.y, c.ClipRect.z, c.ClipRect.w, c.TextureId);
        }
    }

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName("ImGui/material", Ogre::RGN_INTERNAL);
    if (mat)
    {
        for (Ogre::Technique* t : mat->getTechniques())
        {
            for (Ogre::Pass* p : t->getPasses())
            {
                LogFormat("[RoR|Xbox] DIAG   ImGui material tech scheme='%s' supported=%d vp='%s' fp='%s' tex_units=%d",
                          t->getSchemeName().c_str(), (int)t->isSupported(),
                          p->hasVertexProgram() ? p->getVertexProgramName().c_str() : "-",
                          p->hasFragmentProgram() ? p->getFragmentProgramName().c_str() : "-",
                          (int)p->getNumTextureUnitStates());
            }
        }
    }
    else
    {
        LogFormat("[RoR|Xbox] DIAG   ImGui material NOT FOUND");
    }
}

} // namespace

void BeforeRender(Ogre::RenderWindow* window)
{
    const unsigned long now = Clock().getMilliseconds();

    if (!g_listener_added && window)
    {
        window->addListener(&g_grabber);
        g_listener_added = true;
    }

    if (g_next_shot < 2 && now >= SHOT_MS[g_next_shot])
    {
        try { DumpState(window); }
        catch (std::exception& e) { LogFormat("[RoR|Xbox] DIAG dump failed: %s", e.what()); }
        char name[64];
        std::snprintf(name, sizeof(name), "xbox-screenshot-%lus.bmp", SHOT_MS[g_next_shot] / 1000);
        g_grabber.path = PathCombine(App::sys_logs_dir->getStr(), name);
        g_grabber.pending = true;
        ++g_next_shot;
    }

    // Test pattern (first 90 s), drawn on top of everything by ImGui itself:
    //   green frame on the screen edges, red square top-left, blue square bottom-right,
    //   yellow text. If these show up in the right places, ImGui geometry reaches the screen
    //   correctly and any remaining problem is in the RoR windows themselves.
    if (now < PATTERN_MS)
    {
        ImGuiIO& io = ImGui::GetIO();
        const float W = io.DisplaySize.x, H = io.DisplaySize.y;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRect(ImVec2(4, 4), ImVec2(W - 4, H - 4), IM_COL32(0, 255, 0, 255), 0.f, 0, 8.f);
        dl->AddRectFilled(ImVec2(40, 40), ImVec2(240, 240), IM_COL32(255, 0, 0, 255));
        dl->AddRectFilled(ImVec2(W - 240, H - 240), ImVec2(W - 40, H - 40), IM_COL32(0, 80, 255, 255));
        char text[160];
        std::snprintf(text, sizeof(text), "RoR Xbox DIAG  %s %s  display %.0fx%.0f", __DATE__, __TIME__, W, H);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 2.f, ImVec2(270, 60), IM_COL32(255, 255, 0, 255), text);
    }
}

void AfterRender(Ogre::RenderWindow*)
{
}

} // namespace XboxDiag
} // namespace RoR

#endif // ROR_PLATFORM_UWP
