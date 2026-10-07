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

/// @file   RTShaderBootstrap.h
/// @brief  RTShader System bootstrap for render systems without fixed-function
///         pipeline (Direct3D11 on Windows / UWP / Xbox Dev Mode).
///
/// Targets OGRE 1.11.6 (the version RoR pins in conanfile.py). Uses the stock
/// OgreBites::SGTechniqueResolverListener as base class, so there is no
/// hand-written override of MaterialManager::Listener with a stale signature.

#pragma once

#include <OgreMaterialManager.h>
#include <OgreSceneManager.h>
#include <OgreSGTechniqueResolverListener.h>
#include <RTShaderSystem/OgreRTShaderSystem.h>

#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace RoR {

class RTShaderBootstrap
{
public:
    static RTShaderBootstrap& Get();

    /// True when the active render system has no fixed-function pipeline (D3D11, GL3+).
    /// Call after Ogre::Root::initialise().
    static bool IsRequired();

    /// Initialize the RTSS. Must be called:
    ///  - AFTER Ogre::Root::initialise() and the first render window exists
    ///    (the shader generator needs the render system capabilities);
    ///  - BEFORE the first frame is rendered.
    /// @param shaderlib_dir   Folder with the OGRE 1.11.6 RTShaderLib (HLSL_Cg, *.cg files).
    /// @param cache_dir       Writable folder for generated shaders + microcode cache
    ///                        (UWP: LocalCacheFolder). Empty = in-memory only.
    /// @return false if RTSS could not be started (caller decides whether that is fatal).
    bool Init(std::string const& shaderlib_dir, std::string const& cache_dir);

    /// Registers the scene manager with the RTSS (lights/fog tracking) and installs
    /// the scheme enforcer which redirects every viewport rendered through it
    /// (main camera, water RTT, envmap cube faces, Caelum, Hydrax, PagedGeometry
    /// impostors, shadow textures ...) to the RTSS material scheme.
    void AttachSceneManager(Ogre::SceneManager* sm);
    void DetachSceneManager(Ogre::SceneManager* sm);

    /// Optional explicit setter for viewports created by RoR itself.
    void AttachViewport(Ogre::Viewport* vp);

    /// Persist the compiled shader microcode (call on suspend and on shutdown).
    void SaveMicrocodeCache();

    void Shutdown();

    bool IsActive() const { return m_active; }

private:
    RTShaderBootstrap() = default;

    /// Wraps the stock OgreBites listener: same behaviour, but never lets an
    /// exception thrown by the RTSS ("Could not create gpu programs from render
    /// state") escape into the render loop. Failing materials get a magenta
    /// shader-based fallback technique and are logged once.
    class SafeResolverListener : public OgreBites::SGTechniqueResolverListener
    {
    public:
        explicit SafeResolverListener(Ogre::RTShader::ShaderGenerator* sg)
            : OgreBites::SGTechniqueResolverListener(sg) {}

        Ogre::Technique* handleSchemeNotFound(unsigned short schemeIndex,
                                              const Ogre::String& schemeName,
                                              Ogre::Material* originalMaterial,
                                              unsigned short lodIndex,
                                              const Ogre::Renderable* rend) override;

        Ogre::Technique* m_fallback_technique = nullptr;

    private:
        std::set<std::string> m_reported; // materials already logged as failing
    };

    /// Forces the RTSS scheme on any viewport that still uses the default scheme.
    /// Fires inside SceneManager::_renderScene() right after setViewport(), i.e.
    /// after MaterialManager::setActiveScheme(vp->getMaterialScheme()).
    class SchemeEnforcer : public Ogre::SceneManager::Listener
    {
    public:
        void preFindVisibleObjects(Ogre::SceneManager* source,
                                   Ogre::SceneManager::IlluminationRenderStage irs,
                                   Ogre::Viewport* vp) override;
    };

    void CreateFallbackMaterial();
    void LoadMicrocodeCache();

    std::unique_ptr<SafeResolverListener> m_resolver;
    SchemeEnforcer                        m_enforcer;
    std::string                           m_cache_dir;
    bool                                  m_active = false;
};

} // namespace RoR
