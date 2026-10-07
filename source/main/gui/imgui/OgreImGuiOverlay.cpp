// This file is part of the OGRE project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at https://www.ogre3d.org/licensing.

#include <imgui.h>

#include <OgreImGuiOverlay.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreRenderSystem.h>
#include <OgreTextureManager.h>
#include <OgreMaterialManager.h>
#include <OgreOverlayManager.h>
#include <OgreFontManager.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>
#include <OgreFont.h>
#include <OgreRenderQueue.h>
#include <OgreFrameListener.h>
#include <OgreRoot.h>
#include <OgreHighLevelGpuProgramManager.h>
#include <OgreHighLevelGpuProgram.h>
#include <OgreGpuProgramParams.h>
#include <OgreLogManager.h>
#include <OgreStringConverter.h>
#include <algorithm>
#include <cstdio>

namespace Ogre
{

ImGuiOverlay::ImGuiOverlay() : Overlay("ImGuiOverlay")
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    io.BackendPlatformName = "OGRE";
}
ImGuiOverlay::~ImGuiOverlay()
{
    ImGui::DestroyContext();
}

void ImGuiOverlay::initialise()
{
    if (!mInitialised)
    {
        mRenderable.initialise();
        mCodePointRanges.clear();
    }
    mInitialised = true;
}

//-----------------------------------------------------------------------------------
void ImGuiOverlay::_findVisibleObjects(Camera* cam, RenderQueue* queue, Viewport* vp)
{
    if (!mVisible)
        return;

    mRenderable._update();
    queue->addRenderable(&mRenderable, RENDER_QUEUE_OVERLAY, mZOrder * 100);
}
//-----------------------------------------------------------------------------------
void ImGuiOverlay::ImGUIRenderable::createMaterial()
{
    mMaterial = MaterialManager::getSingleton().create("ImGui/material", RGN_INTERNAL);
    Pass* mPass = mMaterial->getTechnique(0)->getPass(0);
    mPass->setCullingMode(CULL_NONE);
    mPass->setVertexColourTracking(TVC_DIFFUSE);
    mPass->setSceneBlending(SBT_TRANSPARENT_ALPHA);
    mPass->setSeparateSceneBlendingOperation(SBO_ADD, SBO_ADD);
    mPass->setSeparateSceneBlending(SBF_SOURCE_ALPHA, SBF_ONE_MINUS_SOURCE_ALPHA,
                                    SBF_ONE_MINUS_SOURCE_ALPHA, SBF_ZERO);

    TextureUnitState* mTexUnit = mPass->createTextureUnitState();
    mTexUnit->setTexture(mFontTex);
    mTexUnit->setTextureFiltering(TFO_NONE);

    // Direct3D 11 has no fixed-function pipeline. Instead of letting the RTSS generate a shader
    // (its worldviewproj path collapsed the whole UI into a small square on Xbox), use a tiny
    // dedicated program pair: pixel coords -> NDC via one float4 uniform, colour * texture.
    if (Root::getSingleton().getRenderSystem()->getName().find("Direct3D11") != String::npos)
        attachD3D11Programs(mPass);

    mMaterial->load();
    mMaterial->setLightingEnabled(false);
    mMaterial->setDepthCheckEnabled(false);
}

void ImGuiOverlay::ImGUIRenderable::attachD3D11Programs(Pass* pass)
{
    static const char* VS_SRC =
        "float4 scaleOffset;\n"
        "struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
        "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
        "VSOut main(VSIn i)\n"
        "{\n"
        "    VSOut o;\n"
        "    o.pos = float4(i.pos * scaleOffset.xy + scaleOffset.zw, 0.0, 1.0);\n"
        "    o.col = i.col;\n"
        "    o.uv  = i.uv;\n"
        "    return o;\n"
        "}\n";
    static const char* PS_SRC =
        "Texture2D    fontTex  : register(t0);\n"
        "SamplerState fontSamp : register(s0);\n"
        "float4 main(float4 pos : SV_POSITION, float4 col : COLOR0, float2 uv : TEXCOORD0) : SV_Target\n"
        "{\n"
        "    return col * fontTex.Sample(fontSamp, uv);\n"
        "}\n";

    try
    {
        HighLevelGpuProgramManager& mgr = HighLevelGpuProgramManager::getSingleton();
        HighLevelGpuProgramPtr vs = mgr.createProgram("ImGui/VS_D3D11", RGN_INTERNAL, "hlsl", GPT_VERTEX_PROGRAM);
        vs->setSource(VS_SRC);
        vs->setParameter("entry_point", "main");
        vs->setParameter("target", "vs_4_0");
        vs->load();

        HighLevelGpuProgramPtr ps = mgr.createProgram("ImGui/PS_D3D11", RGN_INTERNAL, "hlsl", GPT_FRAGMENT_PROGRAM);
        ps->setSource(PS_SRC);
        ps->setParameter("entry_point", "main");
        ps->setParameter("target", "ps_4_0");
        ps->load();

        if (vs->hasCompileError() || ps->hasCompileError() || !vs->isSupported() || !ps->isSupported())
        {
            LogManager::getSingleton().logMessage("[ImGui] D3D11 programs failed to compile, using RTSS", LML_CRITICAL);
            return;
        }

        pass->setVertexProgram(vs->getName());
        pass->setFragmentProgram(ps->getName());
        pass->getVertexProgramParameters()->setIgnoreMissingParams(true);
        mHasOwnPrograms = true;
        LogManager::getSingleton().logMessage("[ImGui] Using dedicated D3D11 HLSL programs (no RTSS)");
    }
    catch (Exception& e)
    {
        LogManager::getSingleton().logMessage("[ImGui] D3D11 programs unavailable: " + e.getFullDescription(), LML_CRITICAL);
        pass->setVertexProgram("");
        pass->setFragmentProgram("");
        mHasOwnPrograms = false;
    }
}

ImFont* ImGuiOverlay::addFont(const String& name, const String& group)
{
    FontPtr font = FontManager::getSingleton().getByName(name, group);
    OgreAssert(font, "font does not exist");
    OgreAssert(font->getType() == FT_TRUETYPE, "font must be of FT_TRUETYPE");
    DataStreamPtr dataStreamPtr =
        ResourceGroupManager::getSingleton().openResource(font->getSource(), font->getGroup());
    MemoryDataStream ttfchunk(dataStreamPtr, false); // transfer ownership to imgui

    // convert codepoint ranges for imgui
    CodePointRange cprange;
    for (const auto& r : font->getCodePointRangeList())
    {
        cprange.push_back(r.first);
        cprange.push_back(r.second);
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImWchar* cprangePtr = io.Fonts->GetGlyphRangesAll();
    if (!cprange.empty())
    {
        cprange.push_back(0); // terminate
        mCodePointRanges.push_back(cprange);
        // ptr must persist until createFontTexture
        cprangePtr = mCodePointRanges.back().data();
    }

    ImFontConfig cfg;
    strncpy(cfg.Name, name.c_str(), 40);
    float size_px = font->getTrueTypeSize();
#if defined(ROR_PLATFORM_UWP)
    // 10-foot UI (TV at couch distance): rasterize the font bigger instead of scaling a small
    // bitmap (FontGlobalScale would look blurry). Must match the style scale in GUIManager.
    const float vp_h = (float)OverlayManager::getSingleton().getViewportHeight();
    size_px *= std::max(1.f, vp_h / 540.f);
#endif
    return io.Fonts->AddFontFromMemoryTTF(ttfchunk.getPtr(), (int)ttfchunk.size(), size_px, &cfg,
                                          cprangePtr);
}

void ImGuiOverlay::ImGUIRenderable::createFontTexture()
{
    // Build texture atlas
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty())
        io.Fonts->AddFontDefault();

    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    mFontTex = TextureManager::getSingleton().createManual("ImGui/FontTex", RGN_INTERNAL, TEX_TYPE_2D,
                                                           width, height, 1, 1, PF_BYTE_RGBA);

    mFontTex->getBuffer()->blitFromMemory(PixelBox(Box(0, 0, width, height), PF_BYTE_RGBA, pixels));
}
void ImGuiOverlay::NewFrame(const FrameEvent& evt)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = std::max<float>(
        evt.timeSinceLastFrame,
        1e-4f); // see https://github.com/ocornut/imgui/commit/3c07ec6a6126fb6b98523a9685d1f0f78ca3c40c

    // Read keyboard modifiers inputs
    io.KeyAlt = false;
    io.KeySuper = false;

    OverlayManager& oMgr = OverlayManager::getSingleton();

    // Setup display size (every frame to accommodate for window resizing)
    io.DisplaySize = ImVec2(oMgr.getViewportWidth(), oMgr.getViewportHeight());

    // Start the frame
    ImGui::NewFrame();
}

void ImGuiOverlay::ImGUIRenderable::_update()
{
    if (mMaterial->getSupportedTechniques().empty())
    {
        mMaterial->load(); // Support for adding lights run time
    }

    RenderSystem* rSys = Root::getSingleton().getRenderSystem();
    OverlayManager& oMgr = OverlayManager::getSingleton();

    // Construct projection matrix, taking texel offset corrections in account (important for DirectX9)
    // See also:
    //     - OGRE-API specific hint: http://www.ogre3d.org/forums/viewtopic.php?f=5&p=536881#p536881
    //     - IMGUI Dx9 demo solution:
    //     https://github.com/ocornut/imgui/blob/master/examples/directx9_example/imgui_impl_dx9.cpp#L127-L138
    float texelOffsetX = rSys->getHorizontalTexelOffset();
    float texelOffsetY = rSys->getVerticalTexelOffset();
    float L = texelOffsetX;
    float R = oMgr.getViewportWidth() + texelOffsetX;
    float T = texelOffsetY;
    float B = oMgr.getViewportHeight() + texelOffsetY;

    mXform = Matrix4(2.0f / (R - L), 0.0f, 0.0f, (L + R) / (L - R), 0.0f, -2.0f / (B - T), 0.0f,
                     (T + B) / (B - T), 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
}

bool ImGuiOverlay::ImGUIRenderable::preRender(SceneManager* sm, RenderSystem* rsys)
{
    Viewport* vp = rsys->_getViewport();

    // Instruct ImGui to Render() and process the resulting CmdList-s
    // Adopted from https://bitbucket.org/ChaosCreator/imgui-ogre2.1-binding
    // ... Commentary on OGRE forums: http://www.ogre3d.org/forums/viewtopic.php?f=5&t=89081#p531059
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    int vpWidth = vp->getActualWidth();
    int vpHeight = vp->getActualHeight();

    const Pass* pass = mMaterial->getBestTechnique()->getPass(0);
    TextureUnitState* tu = pass->getTextureUnitState(0);

    // We issue the draw calls ourselves and return false, so SceneManager::_issueRenderOp() skips
    // its "finalise GPU parameter bindings" step (OGRE 1.11 only updates them when preRender()
    // returns true). With the fixed-function pipeline (D3D9/GL) that did not matter; with shaders
    // (D3D11 + RTSS, Xbox) the vertex shader would run with the previous renderable's
    // worldviewproj matrix and the whole UI collapses into a dot. Update and bind them here.
    if (mHasOwnPrograms && pass->hasVertexProgram())
    {
        // pixel -> NDC: x' = x * 2/W - 1, y' = 1 - y * 2/H (draw_data->DisplayPos is (0,0) here)
        const float w = std::max(1.f, draw_data->DisplaySize.x);
        const float h = std::max(1.f, draw_data->DisplaySize.y);
        pass->getVertexProgramParameters()->setNamedConstant(
            "scaleOffset", Vector4(2.f / w, -2.f / h, -1.f - 2.f * draw_data->DisplayPos.x / w,
                                   1.f + 2.f * draw_data->DisplayPos.y / h));
        rsys->bindGpuProgramParameters(GPT_VERTEX_PROGRAM, pass->getVertexProgramParameters(), GPV_ALL);
        if (pass->hasFragmentProgram())
            rsys->bindGpuProgramParameters(GPT_FRAGMENT_PROGRAM, pass->getFragmentProgramParameters(), GPV_ALL);

        static int s_logged = 0;
        if (s_logged++ < 3)
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "[ImGui] own D3D11 programs: display %.0fx%.0f, viewport %dx%d, lists %d",
                     w, h, vpWidth, vpHeight, draw_data->CmdListsCount);
            LogManager::getSingleton().logMessage(msg);
        }
    }
    else if (pass->isProgrammable())
    {
        pass->_updateAutoParams(sm->_getAutoParamDataSource(), GPV_ALL);
        for (GpuProgramType t : {GPT_VERTEX_PROGRAM, GPT_FRAGMENT_PROGRAM})
        {
            if (pass->hasGpuProgram(t))
                rsys->bindGpuProgramParameters(t, pass->getGpuProgramParameters(t), GPV_ALL);
        }
    }

    for (int i = 0; i < draw_data->CmdListsCount; ++i)
    {
        const ImDrawList* draw_list = draw_data->CmdLists[i];
        updateVertexData(draw_list->VtxBuffer, draw_list->IdxBuffer);

        unsigned int startIdx = 0;

        for (int j = 0; j < draw_list->CmdBuffer.Size; ++j)
        {
            // Create a renderable and fill it's buffers
            const ImDrawCmd* drawCmd = &draw_list->CmdBuffer[j];

            // Set scissoring
            Rect scissor(drawCmd->ClipRect.x, drawCmd->ClipRect.y, drawCmd->ClipRect.z,
                          drawCmd->ClipRect.w);

            // Clamp bounds to viewport dimensions
            scissor = scissor.intersect(Rect(0, 0, vpWidth, vpHeight));

            if (drawCmd->TextureId)
            {
                auto handle = (ResourceHandle)drawCmd->TextureId;
                auto tex = static_pointer_cast<Texture>(TextureManager::getSingleton().getByHandle(handle));
                if (tex)
                {
                    rsys->_setTexture(0, true, tex);
                    rsys->_setSampler(0, *TextureManager::getSingleton().getDefaultSampler());
                }
            }

            rsys->setScissorTest(true, scissor.left, scissor.top, scissor.right, scissor.bottom);

            // Render!
            mRenderOp.indexData->indexStart = startIdx;
            mRenderOp.indexData->indexCount = drawCmd->ElemCount;

            rsys->_render(mRenderOp);

            if (drawCmd->TextureId)
            {
                // reset to pass state
                rsys->_setTexture(0, true, mFontTex);
                rsys->_setSampler(0, *tu->getSampler());
            }

            // Update counts
            startIdx += drawCmd->ElemCount;
        }
    }
    rsys->setScissorTest(false);
    return false;
}

const LightList& ImGuiOverlay::ImGUIRenderable::getLights() const
{
    // Overlayelements should not be lit by the scene, this will not get called
    static LightList ll;
    return ll;
}

ImGuiOverlay::ImGUIRenderable::ImGUIRenderable()
{
    // default overlays to preserve their own detail level
    mPolygonModeOverrideable = false;

    // use identity projection and view matrices
    mUseIdentityProjection = true;
    mUseIdentityView = true;

    mConvertToBGR = false;
}
//-----------------------------------------------------------------------------------
void ImGuiOverlay::ImGUIRenderable::initialise(void)
{
    createFontTexture();
    createMaterial();

    mRenderOp.vertexData = OGRE_NEW VertexData();
    mRenderOp.indexData = OGRE_NEW IndexData();

    mRenderOp.vertexData->vertexCount = 0;
    mRenderOp.vertexData->vertexStart = 0;

    mRenderOp.indexData->indexCount = 0;
    mRenderOp.indexData->indexStart = 0;
    mRenderOp.operationType = RenderOperation::OT_TRIANGLE_LIST;
    mRenderOp.useIndexes = true;
    mRenderOp.useGlobalInstancingVertexBufferIsAvailable = false;

    VertexDeclaration* decl = mRenderOp.vertexData->vertexDeclaration;

    // vertex declaration
    size_t offset = 0;
    decl->addElement(0, offset, VET_FLOAT2, VES_POSITION);
    offset += VertexElement::getTypeSize(VET_FLOAT2);
    decl->addElement(0, offset, VET_FLOAT2, VES_TEXTURE_COORDINATES, 0);
    offset += VertexElement::getTypeSize(VET_FLOAT2);
    decl->addElement(0, offset, VET_COLOUR, VES_DIFFUSE);

    if (Root::getSingleton().getRenderSystem()->getName().find("Direct3D9") != String::npos)
        mConvertToBGR = true;
}
//-----------------------------------------------------------------------------------
ImGuiOverlay::ImGUIRenderable::~ImGUIRenderable()
{
    OGRE_DELETE mRenderOp.vertexData;
    OGRE_DELETE mRenderOp.indexData;
}
//-----------------------------------------------------------------------------------
void ImGuiOverlay::ImGUIRenderable::updateVertexData(const ImVector<ImDrawVert>& vtxBuf,
                                                     const ImVector<ImDrawIdx>& idxBuf)
{
    VertexBufferBinding* bind = mRenderOp.vertexData->vertexBufferBinding;

    if (bind->getBindings().empty() || bind->getBuffer(0)->getNumVertices() != size_t(vtxBuf.size()))
    {
        bind->setBinding(0, HardwareBufferManager::getSingleton().createVertexBuffer(
                                sizeof(ImDrawVert), vtxBuf.size(), HardwareBuffer::HBU_WRITE_ONLY));
    }
    if (!mRenderOp.indexData->indexBuffer ||
        mRenderOp.indexData->indexBuffer->getNumIndexes() != size_t(idxBuf.size()))
    {
        mRenderOp.indexData->indexBuffer = HardwareBufferManager::getSingleton().createIndexBuffer(
            HardwareIndexBuffer::IT_32BIT, idxBuf.size(), HardwareBuffer::HBU_WRITE_ONLY);
    }

    if (mConvertToBGR)
    {
        // convert RGBA > BGRA
        PixelBox src(1, vtxBuf.size(), 1, PF_A8B8G8R8, (char*)vtxBuf.Data + offsetof(ImDrawVert, col));
        src.rowPitch = sizeof(ImDrawVert) / sizeof(ImU32);
        PixelBox dst = src;
        dst.format = PF_A8R8G8B8;
        PixelUtil::bulkPixelConversion(src, dst);
    }

    // Copy all vertices
    bind->getBuffer(0)->writeData(0, vtxBuf.size_in_bytes(), vtxBuf.Data, true);
    mRenderOp.indexData->indexBuffer->writeData(0, idxBuf.size_in_bytes(), idxBuf.Data, true);

    mRenderOp.vertexData->vertexStart = 0;
    mRenderOp.vertexData->vertexCount = vtxBuf.size();
}
} // namespace Ogre
