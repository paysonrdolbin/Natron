/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2026 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "OCIOViewerProcess.h"

#include <cstdlib> // getenv
#include <map>
#include <utility>

CLANG_DIAG_OFF(deprecated)
#include <QMutex>
CLANG_DIAG_ON(deprecated)

#ifdef NATRON_USE_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

NATRON_NAMESPACE_ENTER

#ifdef NATRON_USE_OCIO

namespace {

class OCIOViewerProcessorImpl
    : public OCIOViewerProcessor
{
public:
    OCIOViewerProcessorImpl(const OCIO::ConstProcessorRcPtr& processor)
        : _cacheID( processor->getCacheID() )
        , _cpu( processor->getDefaultCPUProcessor() )
    {
        OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        // GLSL 1.20 is what the viewer's legacy OpenGL context runs (OpenGL 2.1 on macOS)
        desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_2);
        desc->setFunctionName("OCIODisplay");
        desc->setResourcePrefix("ocio_");
        desc->setAllowTexture1D(false); // 1D tables as Nx1 2D textures
        processor->getDefaultGPUProcessor()->extractGpuShaderInfo(desc);

        _shader.functionName = desc->getFunctionName();
        _shader.shaderText = desc->getShaderText();
        for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
            const char* textureName = 0;
            const char* samplerName = 0;
            unsigned width = 0, height = 0;
            OCIO::GpuShaderDesc::TextureType channel = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
            OCIO::GpuShaderDesc::TextureDimensions dimensions = OCIO::GpuShaderDesc::TEXTURE_2D;
            OCIO::Interpolation interpolation = OCIO::INTERP_LINEAR;
            desc->getTexture(i, textureName, samplerName, width, height, channel, dimensions, interpolation);
            const float* values = 0;
            desc->getTextureValues(i, values);

            OCIOViewerShader::Texture t;
            t.samplerName = samplerName;
            t.width = width;
            t.height = height;
            t.channels = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? 1 : 3;
            t.linearInterpolation = (interpolation != OCIO::INTERP_NEAREST);
            t.values.assign(values, values + (size_t)width * height * t.channels);
            _shader.textures.push_back(t);
        }
        for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
            const char* textureName = 0;
            const char* samplerName = 0;
            unsigned edgeLen = 0;
            OCIO::Interpolation interpolation = OCIO::INTERP_LINEAR;
            desc->get3DTexture(i, textureName, samplerName, edgeLen, interpolation);
            const float* values = 0;
            desc->get3DTextureValues(i, values);

            OCIOViewerShader::Texture t;
            t.samplerName = samplerName;
            t.width = t.height = t.depth = edgeLen;
            t.channels = 3;
            t.linearInterpolation = (interpolation != OCIO::INTERP_NEAREST);
            t.values.assign(values, values + (size_t)edgeLen * edgeLen * edgeLen * 3);
            _shader.textures.push_back(t);
        }
    }

    virtual const std::string& getCacheID() const OVERRIDE FINAL
    {
        return _cacheID;
    }

    virtual void applyRGBA(float* pixels,
                           long numPixels) const OVERRIDE FINAL
    {
        OCIO::PackedImageDesc img(pixels, numPixels, 1, 4);

        _cpu->apply(img);
    }

    virtual const OCIOViewerShader& getShader() const OVERRIDE FINAL
    {
        return _shader;
    }

private:
    std::string _cacheID;
    OCIO::ConstCPUProcessorRcPtr _cpu;
    OCIOViewerShader _shader;
};

struct ConfigState
{
    QMutex mutex;
    std::string configPath; // value of OCIO when the config was loaded
    std::string error;
    OCIO::ConstConfigRcPtr config;
    std::map<std::pair<std::string, std::string>, OCIOViewerProcessorPtr> processors;
};

ConfigState&
state()
{
    static ConfigState s;

    return s;
}

// Must be called with state().mutex locked.
bool
loadCurrentConfigLocked(std::string* error)
{
    ConfigState& s = state();
    const char* env = std::getenv("OCIO");
    const std::string path = env ? env : "";

    if ( (path != s.configPath) || (!s.config && s.error.empty()) ) {
        s.configPath = path;
        s.config.reset();
        s.processors.clear();
        s.error.clear();
        if ( path.empty() ) {
            s.error = "The OCIO environment variable is not set.";
        } else {
            try {
                // also accepts built-in configs, e.g. ocio://studio-config-latest
                s.config = OCIO::Config::CreateFromFile( path.c_str() );
            } catch (const OCIO::Exception& e) {
                s.error = e.what();
            }
        }
    }
    if (error) {
        *error = s.error;
    }

    return (bool)s.config;
}
} // anon namespace

bool
OCIOViewerProcess::isAvailable()
{
    return true;
}

bool
OCIOViewerProcess::loadCurrentConfig(std::string* error)
{
    QMutexLocker k(&state().mutex);

    return loadCurrentConfigLocked(error);
}

std::string
OCIOViewerProcess::getConfigName()
{
    QMutexLocker k(&state().mutex);

    if ( !loadCurrentConfigLocked(0) ) {
        return std::string();
    }

    return state().config->getName();
}

bool
OCIOViewerProcess::isBuiltinConfig()
{
    QMutexLocker k(&state().mutex);

    return loadCurrentConfigLocked(0) && (state().configPath.compare(0, 7, "ocio://") == 0);
}

std::vector<std::string>
OCIOViewerProcess::getDisplays()
{
    QMutexLocker k(&state().mutex);
    std::vector<std::string> ret;

    if ( !loadCurrentConfigLocked(0) ) {
        return ret;
    }
    const OCIO::ConstConfigRcPtr& config = state().config;
    for (int i = 0; i < config->getNumDisplays(); ++i) {
        ret.push_back( config->getDisplay(i) );
    }

    return ret;
}

std::string
OCIOViewerProcess::getDefaultDisplay()
{
    QMutexLocker k(&state().mutex);

    if ( !loadCurrentConfigLocked(0) ) {
        return std::string();
    }

    return state().config->getDefaultDisplay();
}

std::vector<std::string>
OCIOViewerProcess::getViews(const std::string& display)
{
    QMutexLocker k(&state().mutex);
    std::vector<std::string> ret;

    if ( !loadCurrentConfigLocked(0) ) {
        return ret;
    }
    const OCIO::ConstConfigRcPtr& config = state().config;
    for (int i = 0; i < config->getNumViews( display.c_str() ); ++i) {
        ret.push_back( config->getView(display.c_str(), i) );
    }

    return ret;
}

std::string
OCIOViewerProcess::getDefaultView(const std::string& display)
{
    QMutexLocker k(&state().mutex);

    if ( !loadCurrentConfigLocked(0) ) {
        return std::string();
    }

    return state().config->getDefaultView( display.c_str() );
}

OCIOViewerProcessorPtr
OCIOViewerProcess::getProcessor(const std::string& display,
                                const std::string& view,
                                std::string* error)
{
    QMutexLocker k(&state().mutex);
    ConfigState& s = state();

    if ( !loadCurrentConfigLocked(error) ) {
        return OCIOViewerProcessorPtr();
    }
    const std::pair<std::string, std::string> key(display, view);
    std::map<std::pair<std::string, std::string>, OCIOViewerProcessorPtr>::const_iterator found = s.processors.find(key);
    if ( found != s.processors.end() ) {
        return found->second;
    }

    OCIOViewerProcessorPtr ret;
    try {
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc(OCIO::ROLE_SCENE_LINEAR); // Natron's working space
        transform->setDisplay( display.c_str() );
        transform->setView( view.c_str() );
        ret = std::make_shared<OCIOViewerProcessorImpl>( s.config->getProcessor(transform) );
    } catch (const OCIO::Exception& e) {
        if (error) {
            *error = e.what();
        }

        return OCIOViewerProcessorPtr();
    }
    s.processors[key] = ret;

    return ret;
}

#else // !NATRON_USE_OCIO

bool
OCIOViewerProcess::isAvailable()
{
    return false;
}

bool
OCIOViewerProcess::loadCurrentConfig(std::string* error)
{
    if (error) {
        *error = "Natron was built without OpenColorIO support.";
    }

    return false;
}

std::string
OCIOViewerProcess::getConfigName()
{
    return std::string();
}

bool
OCIOViewerProcess::isBuiltinConfig()
{
    return false;
}

std::vector<std::string>
OCIOViewerProcess::getDisplays()
{
    return std::vector<std::string>();
}

std::string
OCIOViewerProcess::getDefaultDisplay()
{
    return std::string();
}

std::vector<std::string>
OCIOViewerProcess::getViews(const std::string& /*display*/)
{
    return std::vector<std::string>();
}

std::string
OCIOViewerProcess::getDefaultView(const std::string& /*display*/)
{
    return std::string();
}

OCIOViewerProcessorPtr
OCIOViewerProcess::getProcessor(const std::string& /*display*/,
                                const std::string& /*view*/,
                                std::string* error)
{
    loadCurrentConfig(error);

    return OCIOViewerProcessorPtr();
}

#endif // NATRON_USE_OCIO

NATRON_NAMESPACE_EXIT
