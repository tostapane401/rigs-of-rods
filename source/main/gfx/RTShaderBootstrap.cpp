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

#include "RTShaderBootstrap.h"

#include "Application.h"
#include "PlatformUtils.h"

#include <OgreGpuProgramManager.h>
#include <OgreHighLevelGpuProgram.h>
#include <OgreHighLevelGpuProgramManager.h>
#include <OgreRenderQueue.h>
#include <OgreRenderSystem.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreTechnique.h>
#include <OgreViewport.h>

#include <fmt/format.h>
#include <fstream>

using namespace Ogre;

namespace RoR {

namespace {

const char* RGN_RTSHADERLIB    = "RtShaderRG";            // same name as ContentManager::ResourcePack::RTSHADER
const char* FALLBACK_MAT_NAME  = "RoR/RTSS/Fallback";
const char* MICROCODE_FILENAME = "shader_microcode.cache";

bool TechniqueIsProgrammable(const Technique* tech)
{
    if (!tech || tech->getNumPasses() == 0)
        return false;
    for (unsigned short i = 0; i < tech->getNumPasses(); ++i)
    {
        // D3D11 refuses to draw without a vertex AND a fragment shader.
        const Pass* p = tech->getPass(i);
        if (!p->hasVertexProgram() || !p->hasFragmentProgram())
            return false;
    }
    return true;
}

/// Last line of defence: the RenderQueue asks this listener for every renderable
/// it receives. If, for any reason (custom viewport scheme set by a plugin, a
/// render target updated before the enforcer saw it, a material with only Cg
/// techniques), a fixed-function technique reaches the queue, swap it for the
/// RTSS one or for the fallback - instead of letting D3D11 throw
/// "Attempted to render to a D3D11 device without a vertex shader".
class QueueGuard : public RenderQueue::RenderableListener
{
public:
    Technique* fallback = nullptr;

    bool renderableQueued(Renderable* rend, uint8 groupID, ushort priority,
                          Technique** ppTech, RenderQueue* pQueue) override
    {
        (void)rend; (void)groupID; (void)priority; (void)pQueue;
        if (!ppTech || TechniqueIsProgrammable(*ppTech))
            return true;

        Material* mat = (*ppTech) ? (*ppTech)->getParent() : nullptr;
        if (mat)
        {
            MaterialManager& mm = MaterialManager::getSingleton();
            const String prev_scheme = mm.getActiveScheme();
            if (prev_scheme != RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME)
            {
                mm.setActiveScheme(RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
                Technique* t = mat->getBestTechnique(); // goes through SafeResolverListener
                mm.setActiveScheme(prev_scheme);
                if (TechniqueIsProgrammable(t))
                {
                    *ppTech = t;
                    return true;
                }
            }
        }
        if (fallback)
        {
            *ppTech = fallback;
            return true;
        }
        return false; // Skip the renderable rather than crash the device.
    }
};

QueueGuard g_queue_guard;

} // anonymous namespace

// -------------------------------------------------------------------------------------------------
// SafeResolverListener
// -------------------------------------------------------------------------------------------------

Technique* RTShaderBootstrap::SafeResolverListener::handleSchemeNotFound(
    unsigned short schemeIndex, const String& schemeName, Material* originalMaterial,
    unsigned short lodIndex, const Renderable* rend)
{
    if (schemeName != RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME || !originalMaterial)
        return nullptr;

    // Materials that already failed once: do not retry every frame.
    const std::string key = originalMaterial->getGroup() + "/" + originalMaterial->getName();
    if (m_reported.count(key))
        return m_fallback_technique;

    try
    {
        // Stock OgreBites behaviour: createShaderBasedTechnique() + validateMaterial().
        Technique* t = OgreBites::SGTechniqueResolverListener::handleSchemeNotFound(
            schemeIndex, schemeName, originalMaterial, lodIndex, rend);
        if (t)
            return t;

        // nullptr means either "material is already programmable" (fine, Ogre falls back to
        // the default scheme technique) or "RTSS refused it". Distinguish the two.
        Technique* best_default = nullptr;
        for (Technique* tech : originalMaterial->getSupportedTechniques())
        {
            if (tech->getSchemeName() == MaterialManager::DEFAULT_SCHEME_NAME)
            {
                best_default = tech;
                break;
            }
        }
        if (TechniqueIsProgrammable(best_default))
            return nullptr;

        RoR::LogFormat("[RoR|RTSS] Material '%s' (group '%s') has no shader technique and RTSS could not "
                       "generate one -> using fallback.", originalMaterial->getName().c_str(),
                       originalMaterial->getGroup().c_str());
    }
    catch (Ogre::Exception& e)
    {
        // Typical message: "Could not create gpu programs from render state".
        // The real reason (HLSL compile error) is logged by the D3D11 render system just above.
        RoR::LogFormat("[RoR|RTSS] Shader generation failed for material '%s' (group '%s'): %s",
                       originalMaterial->getName().c_str(), originalMaterial->getGroup().c_str(),
                       e.getFullDescription().c_str());
    }
    m_reported.insert(key);
    return m_fallback_technique;
}

// -------------------------------------------------------------------------------------------------
// SchemeEnforcer
// -------------------------------------------------------------------------------------------------

void RTShaderBootstrap::SchemeEnforcer::preFindVisibleObjects(
    SceneManager* source, SceneManager::IlluminationRenderStage irs, Viewport* vp)
{
    (void)source; (void)irs;
    if (vp && vp->getMaterialScheme() == MaterialManager::DEFAULT_SCHEME_NAME)
    {
        vp->setMaterialScheme(RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
        // setViewport() already ran for this pass -> also patch the active scheme for this frame.
        MaterialManager::getSingleton().setActiveScheme(RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
    }
}

// -------------------------------------------------------------------------------------------------
// RTShaderBootstrap
// -------------------------------------------------------------------------------------------------

RTShaderBootstrap& RTShaderBootstrap::Get()
{
    static RTShaderBootstrap instance;
    return instance;
}

bool RTShaderBootstrap::IsRequired()
{
    RenderSystem* rs = Root::getSingleton().getRenderSystem();
    if (!rs || !rs->getCapabilities())
        return false;
    return !rs->getCapabilities()->hasCapability(RSC_FIXED_FUNCTION);
}

bool RTShaderBootstrap::Init(std::string const& shaderlib_dir, std::string const& cache_dir)
{
    if (m_active)
        return true;

    ResourceGroupManager& rgm = ResourceGroupManager::getSingleton();

    // 1) Shader library. MUST be the OGRE 1.11.6 'Samples/Media/RTShaderLib/HLSL_Cg' set
    //    (FFPLib_AlphaTest.cg & co.). The pre-1.11 library previously shipped in
    //    resources/rtshader lacks FFP_Alpha_Test / FFP_Normalize / FFP_PixelFog_PositionDepth,
    //    so every generated HLSL program failed to compile on D3D11.
    if (!FolderExists(shaderlib_dir) && !FileExists(shaderlib_dir))
    {
        RoR::LogFormat("[RoR|RTSS] Shader library not found at '%s'", shaderlib_dir.c_str());
        return false;
    }
    if (!rgm.resourceGroupExists(RGN_RTSHADERLIB))
    {
        const bool is_zip = FileExists(shaderlib_dir);
        rgm.addResourceLocation(shaderlib_dir, is_zip ? "Zip" : "FileSystem", RGN_RTSHADERLIB);
        rgm.initialiseResourceGroup(RGN_RTSHADERLIB);
    }
    if (!rgm.resourceExists(RGN_RTSHADERLIB, "FFPLib_AlphaTest.cg"))
    {
        RoR::Log("[RoR|RTSS] WARNING: 'FFPLib_AlphaTest.cg' missing - the shader library does not match "
                 "OGRE 1.11.6, generated shaders will fail to compile.");
    }

    // 2) Start the generator (needs initialised render system).
    if (!RTShader::ShaderGenerator::initialize())
    {
        RoR::Log("[RoR|RTSS] ShaderGenerator::initialize() failed");
        return false;
    }
    RTShader::ShaderGenerator* sg = RTShader::ShaderGenerator::getSingletonPtr();

    // 'hlsl' is auto-selected on D3D11 because the Cg plugin is not loaded on UWP/Xbox.
    // If Cg is loaded on desktop, make sure we still emit plain HLSL (Cg runtime does not exist on Xbox).
    if (HighLevelGpuProgramManager::getSingleton().isLanguageSupported("hlsl"))
        sg->setTargetLanguage("hlsl");

    // 3) Disk cache. ShaderGenerator::setShaderCachePath() throws if the path is not writable
    //    (exactly what happens with 'Documents\My Games' inside the UWP sandbox), and
    //    ProgramManager::createGpuProgram() returns null on a failed write -> which surfaces as
    //    "Could not create gpu programs from render state". Never let that happen: probe first.
    m_cache_dir.clear();
    if (!cache_dir.empty())
    {
        const std::string rtss_cache = PathCombine(cache_dir, "rtshader");
        CreateFolder(cache_dir);
        CreateFolder(rtss_cache);
        try
        {
            sg->setShaderCachePath(rtss_cache);
            m_cache_dir = cache_dir;
        }
        catch (Ogre::Exception& e)
        {
            RoR::LogFormat("[RoR|RTSS] Shader cache disabled (path not writable): %s", e.getDescription().c_str());
            sg->setShaderCachePath("");
        }
    }

    // 4) Microcode cache: avoids recompiling HLSL with D3DCompile on every launch
    //    (big win on Xbox One CPUs).
    if (!m_cache_dir.empty() && GpuProgramManager::canGetCompiledShaderBuffer())
    {
        GpuProgramManager::getSingleton().setSaveMicrocodesToCache(true);
        this->LoadMicrocodeCache();
    }

    // 5) Fallback technique + resolver listener (stock OgreBites listener, wrapped).
    this->CreateFallbackMaterial();
    m_resolver.reset(new SafeResolverListener(sg));
    MaterialPtr fallback = MaterialManager::getSingleton().getByName(FALLBACK_MAT_NAME, ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    m_resolver->m_fallback_technique = fallback ? fallback->getBestTechnique() : nullptr;
    g_queue_guard.fallback = m_resolver->m_fallback_technique;
    MaterialManager::getSingleton().addListener(m_resolver.get());

    // Code that queries techniques outside of rendering (e.g. getBestTechnique() in loaders)
    // should already see the RTSS scheme.
    MaterialManager::getSingleton().setActiveScheme(RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);

    m_active = true;
    RoR::LogFormat("[RoR|RTSS] RTShader System active (language '%s', cache '%s')",
                   sg->getTargetLanguage().c_str(), sg->getShaderCachePath().c_str());
    return true;
}

void RTShaderBootstrap::CreateFallbackMaterial()
{
    // Hand-written, dependency-free HLSL (no RTSS library involved), so that it is guaranteed to
    // compile even when the RTSS library is broken. Renders unconverted materials magenta.
    auto& hlmgr = HighLevelGpuProgramManager::getSingleton();
    if (!hlmgr.isLanguageSupported("hlsl"))
        return; // GL3+: RTSS fallback via GLSL not needed for the Xbox target.

    const String grp = ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME;
    if (MaterialManager::getSingleton().resourceExists(FALLBACK_MAT_NAME, grp))
        return;

    try
    {
        HighLevelGpuProgramPtr vs = hlmgr.createProgram("RoR/RTSS/Fallback_VS", grp, "hlsl", GPT_VERTEX_PROGRAM);
        vs->setSource(
            "uniform float4x4 worldViewProj;\n"
            "void main(float4 pos : POSITION, out float4 oPos : SV_POSITION)\n"
            "{ oPos = mul(worldViewProj, pos); }\n");
        vs->setParameter("entry_point", "main");
        vs->setParameter("target", "vs_4_0");
        vs->load();

        HighLevelGpuProgramPtr ps = hlmgr.createProgram("RoR/RTSS/Fallback_PS", grp, "hlsl", GPT_FRAGMENT_PROGRAM);
        ps->setSource(
            "uniform float4 colour;\n"
            "float4 main(float4 pos : SV_POSITION) : SV_TARGET\n"
            "{ return colour; }\n");
        ps->setParameter("entry_point", "main");
        ps->setParameter("target", "ps_4_0");
        ps->load();

        MaterialPtr mat = MaterialManager::getSingleton().create(FALLBACK_MAT_NAME, grp);
        Technique* tech = mat->getTechnique(0);
        // Belongs to the RTSS scheme so that SceneManager accepts it as "best technique".
        tech->setSchemeName(RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
        Pass* pass = tech->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexProgram(vs->getName());
        pass->setFragmentProgram(ps->getName());
        pass->getVertexProgramParameters()->setNamedAutoConstant("worldViewProj", GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
        pass->getFragmentProgramParameters()->setNamedConstant("colour", ColourValue(1.f, 0.f, 1.f, 1.f));
        mat->load();
    }
    catch (Ogre::Exception& e)
    {
        RoR::LogFormat("[RoR|RTSS] Could not create fallback material: %s", e.getFullDescription().c_str());
    }
}

void RTShaderBootstrap::AttachSceneManager(SceneManager* sm)
{
    if (!m_active || !sm)
        return;
    RTShader::ShaderGenerator::getSingleton().addSceneManager(sm); // lights, fog, shadows tracking
    sm->addListener(&m_enforcer);
    sm->getRenderQueue()->setRenderableListener(&g_queue_guard);
}

void RTShaderBootstrap::DetachSceneManager(SceneManager* sm)
{
    if (!m_active || !sm)
        return;
    sm->removeListener(&m_enforcer);
    if (sm->getRenderQueue()->getRenderableListener() == &g_queue_guard)
        sm->getRenderQueue()->setRenderableListener(nullptr);
    RTShader::ShaderGenerator::getSingleton().removeSceneManager(sm);
}

void RTShaderBootstrap::AttachViewport(Viewport* vp)
{
    if (m_active && vp)
        vp->setMaterialScheme(RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
}

void RTShaderBootstrap::LoadMicrocodeCache()
{
    const std::string path = PathCombine(m_cache_dir, MICROCODE_FILENAME);
    if (!FileExists(path))
        return;
    try
    {
        std::ifstream* f = OGRE_NEW_T(std::ifstream, MEMCATEGORY_GENERAL)(path.c_str(), std::ios::in | std::ios::binary);
        DataStreamPtr stream(OGRE_NEW FileStreamDataStream(f, /*freeOnClose=*/true));
        GpuProgramManager::getSingleton().loadMicrocodeCache(stream);
        RoR::LogFormat("[RoR|RTSS] Loaded shader microcode cache '%s'", path.c_str());
    }
    catch (Ogre::Exception& e)
    {
        RoR::LogFormat("[RoR|RTSS] Ignoring corrupt microcode cache: %s", e.getDescription().c_str());
    }
}

void RTShaderBootstrap::SaveMicrocodeCache()
{
    if (!m_active || m_cache_dir.empty())
        return;
    GpuProgramManager& gpm = GpuProgramManager::getSingleton();
    if (!gpm.getSaveMicrocodesToCache() || !gpm.isCacheDirty())
        return;
    const std::string path = PathCombine(m_cache_dir, MICROCODE_FILENAME);
    try
    {
        std::fstream* f = OGRE_NEW_T(std::fstream, MEMCATEGORY_GENERAL)(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        DataStreamPtr stream(OGRE_NEW FileStreamDataStream(f, /*freeOnClose=*/true));
        gpm.saveMicrocodeCache(stream);
    }
    catch (Ogre::Exception& e)
    {
        RoR::LogFormat("[RoR|RTSS] Could not save microcode cache: %s", e.getDescription().c_str());
    }
}

void RTShaderBootstrap::Shutdown()
{
    if (!m_active)
        return;
    this->SaveMicrocodeCache();
    if (m_resolver)
        MaterialManager::getSingleton().removeListener(m_resolver.get());
    m_resolver.reset();
    RTShader::ShaderGenerator::destroy();
    m_active = false;
}

} // namespace RoR
