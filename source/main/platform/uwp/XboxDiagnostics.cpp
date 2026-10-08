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
#include "XboxInput.h"
#include "GameContext.h"
#include "Actor.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <MyGUI.h>
#include <MyGUI_OgreRenderManager.h>

#include <OgreHighLevelGpuProgram.h>
#include <OgreHighLevelGpuProgramManager.h>

#include <OgreMaterialManager.h>
#include <OgreOverlayManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayElement.h>
#include <OgreRenderWindow.h>
#include <OgreRoot.h>
#include <OgreTechnique.h>
#include <OgreTimer.h>
#include <OgreViewport.h>

#include <d3d11.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <exception>
#include <vector>

namespace RoR {
namespace XboxDiag {

namespace {

const unsigned long STATUS_MS = 600000;       // on-screen input status lifetime
const unsigned long DUMP_MS[2] = {8000, 30000}; // state dump times (RoR.log)

Ogre::Timer& Clock()
{
    static Ogre::Timer t;
    return t;
}

int g_next_dump = 0;

void DumpWidget(MyGUI::Widget* w, int depth)
{
    if (!w || !w->getVisible())
        return;
    const MyGUI::IntCoord c = w->getAbsoluteCoord();
    LogFormat("[RoR|Xbox] DIAG   %*smygui %s '%s' abs=(%d,%d %dx%d) alpha=%.2f", depth * 2, "", w->getTypeName().c_str(),
              w->getName().c_str(), c.left, c.top, c.width, c.height, w->getAlpha());
    if (depth < 3)
    {
        for (size_t i = 0; i < w->getChildCount(); ++i)
            DumpWidget(w->getChildAt(i), depth + 1);
    }
}

void DumpOverlayElement(Ogre::OverlayElement* e, int depth)
{
    if (!e)
        return;
    LogFormat("[RoR|Xbox] DIAG   %*soverlay-elem %s '%s' visible=%d rel=(%.3f,%.3f %.3fx%.3f) mat='%s'", depth * 2, "",
              e->getTypeName().c_str(), e->getName().c_str(), (int)e->isVisible(), e->_getDerivedLeft(),
              e->_getDerivedTop(), e->_getRelativeWidth(), e->_getRelativeHeight(), e->getMaterialName().c_str());
    if (depth < 3 && e->isContainer())
    {
        auto it = static_cast<Ogre::OverlayContainer*>(e)->getChildIterator();
        while (it.hasMoreElements())
            DumpOverlayElement(it.getNext(), depth + 1);
    }
}

void DumpState(Ogre::RenderWindow* window)
{
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = g.IO;
    Ogre::OverlayManager& om = Ogre::OverlayManager::getSingleton();

    LogFormat("[RoR|Xbox] DIAG build %s %s", __DATE__, __TIME__);
    LogFormat("[RoR|Xbox] DIAG window %ux%u viewports=%d overlay-vp=%dx%d", window->getWidth(), window->getHeight(),
              (int)window->getNumViewports(), om.getViewportWidth(), om.getViewportHeight());
    // The real swap-chain back buffer (Ogre's own width/height are only what it asked for).
    ID3D11Texture2D* bb = nullptr;
    window->getCustomAttribute("ID3D11Texture2D", &bb);
    if (bb)
    {
        D3D11_TEXTURE2D_DESC d = {};
        bb->GetDesc(&d);
        LogFormat("[RoR|Xbox] DIAG back buffer %ux%u format=%d samples=%u%s", d.Width, d.Height, (int)d.Format,
                  d.SampleDesc.Count, (d.Width != window->getWidth() || d.Height != window->getHeight()) ? "  <-- SIZE MISMATCH" : "");
    }
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

    // Everything else that can put pixels on screen in the main menu: Ogre overlays and MyGUI.
    auto ov = om.getOverlayIterator();
    while (ov.hasMoreElements())
    {
        Ogre::Overlay* o = ov.getNext();
        if (!o->isVisible())
            continue;
        LogFormat("[RoR|Xbox] DIAG   overlay '%s' zorder=%u", o->getName().c_str(), (unsigned)o->getZOrder());
        for (Ogre::OverlayContainer* c : o->get2DElements())
            DumpOverlayElement(c, 1);
    }

    if (MyGUI::Gui::getInstancePtr())
    {
        LogFormat("[RoR|Xbox] DIAG   mygui pointer visible=%d batches(last frame)=%u", (int)MyGUI::PointerManager::getInstance().isVisible(),
                  (unsigned)MyGUI::OgreRenderManager::getInstance().getBatchCount());
        const MyGUI::IntSize mvs = MyGUI::RenderManager::getInstance().getViewSize();
        LogFormat("[RoR|Xbox] DIAG   mygui view size %dx%d", mvs.width, mvs.height);
        for (const char* prog : {"MyGUI_VP.hlsl", "MyGUI_FP.hlsl"})
        {
            Ogre::HighLevelGpuProgramPtr p = Ogre::HighLevelGpuProgramManager::getSingleton().getByName(prog, "MyGuiRG");
            if (p)
                LogFormat("[RoR|Xbox] DIAG   mygui program %s loaded=%d compile_error=%d supported=%d", prog,
                          (int)p->isLoaded(), (int)p->hasCompileError(), (int)p->isSupported());
            else
                LogFormat("[RoR|Xbox] DIAG   mygui program %s NOT FOUND", prog);
        }
        MyGUI::EnumeratorWidgetPtr it = MyGUI::Gui::getInstance().getEnumerator();
        while (it.next())
            DumpWidget(it.current(), 0);
    }
}

} // namespace

void BeforeRender(Ogre::RenderWindow* window)
{
    const unsigned long now = Clock().getMilliseconds();

    // One more dump 10 s after first entering a vehicle (HUD / dashboards are MyGUI).
    static unsigned long s_sim_since = 0;
    static bool s_sim_dumped = false;
    if (!s_sim_dumped && App::app_state->getEnum<AppState>() == AppState::SIMULATION &&
        App::GetGameContext()->GetPlayerActor() != nullptr) // driving a vehicle
    {
        if (!s_sim_since)
            s_sim_since = now ? now : 1;
        else if (now - s_sim_since >= 10000)
        {
            s_sim_dumped = true;
            LogFormat("[RoR|Xbox] DIAG ---- in simulation ----");
            try { DumpState(window); }
            catch (std::exception& e) { LogFormat("[RoR|Xbox] DIAG dump failed: %s", e.what()); }
        }
    }

    // MyGUI probe: a green "MyGUI" label at the top right. Visible -> MyGUI rendering works.
    static MyGUI::TextBox* s_probe = nullptr;
    if (!s_probe && MyGUI::Gui::getInstancePtr() && now > 2000)
    {
        try
        {
            const MyGUI::IntSize vs = MyGUI::RenderManager::getInstance().getViewSize();
            s_probe = MyGUI::Gui::getInstance().createWidget<MyGUI::TextBox>("TextBox",
                MyGUI::IntCoord(vs.width - 260, 12, 240, 40), MyGUI::Align::Default, "Popup", "XboxDiagMyGuiProbe");
            s_probe->setCaption("MyGUI");
            s_probe->setTextColour(MyGUI::Colour(0.f, 1.f, 0.f, 1.f));
            s_probe->setFontHeight(32);
            s_probe->setTextAlign(MyGUI::Align::Right);
            s_probe->setNeedMouseFocus(false);
        }
        catch (std::exception& e)
        {
            LogFormat("[RoR|Xbox] DIAG MyGUI probe failed: %s", e.what());
            s_probe = reinterpret_cast<MyGUI::TextBox*>(1); // do not retry
        }
    }

    if (g_next_dump < 2 && now >= DUMP_MS[g_next_dump])
    {
        try { DumpState(window); }
        catch (std::exception& e) { LogFormat("[RoR|Xbox] DIAG dump failed: %s", e.what()); }
        ++g_next_dump;
    }

    // Rendering is confirmed working; during input bring-up show a one-line live input status
    // at the top of the screen (first 10 minutes) and log it every 5 s for 2 minutes.
    const std::string status = XboxInput::DebugStatus();
    static unsigned long s_last_log = 0;
    if (now < 120000 && now - s_last_log >= 5000)
    {
        s_last_log = now;
        LogFormat("[RoR|Xbox] INPUT %s", status.c_str());
    }
    if (now < STATUS_MS)
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float fs = ImGui::GetFontSize();
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, status.c_str());
        dl->AddRectFilled(ImVec2(8, 8), ImVec2(24 + size.x, 16 + size.y), IM_COL32(0, 0, 0, 170), 4.f);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(16, 12), IM_COL32(255, 230, 0, 255), status.c_str());
    }
}

void AfterRender(Ogre::RenderWindow*)
{
}

} // namespace XboxDiag
} // namespace RoR

#endif // ROR_PLATFORM_UWP
