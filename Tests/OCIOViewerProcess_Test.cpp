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

#include "Global/Macros.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <gtest/gtest.h>

#include "Engine/OCIOViewerProcess.h"

#ifdef NATRON_USE_OCIO
#include <OpenColorIO/OpenColorIO.h>
#endif

NATRON_NAMESPACE_USING

#ifdef NATRON_USE_OCIO

namespace {
const char* kACES2Studio = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* kDisplay = "sRGB - Display";
const char* kView = "ACES 2.0 - SDR 100 nits (Rec.709)";

// Sets OCIO for the duration of a test, then restores the previous value.
class ScopedOCIOEnv
{
public:
    explicit ScopedOCIOEnv(const char* value)
    {
        const char* old = std::getenv("OCIO");
        _hadOld = (old != 0);
        if (_hadOld) {
            _old = old;
        }
        setenv("OCIO", value, 1);
    }

    ~ScopedOCIOEnv()
    {
        if (_hadOld) {
            setenv("OCIO", _old.c_str(), 1);
        } else {
            unsetenv("OCIO");
        }
    }

private:
    bool _hadOld;
    std::string _old;
};
} // anon namespace

// the ACES 2.0 built-in configs need OpenColorIO 2.5
#if OCIO_VERSION_HEX >= 0x02050000
TEST(OCIOViewerProcess, ListsACES2DisplaysAndViews)
{
    ScopedOCIOEnv env(kACES2Studio);
    std::string error;
    ASSERT_TRUE( OCIOViewerProcess::loadCurrentConfig(&error) ) << error;
    EXPECT_EQ( std::string("studio-config-v4.0.0_aces-v2.0_ocio-v2.5"), OCIOViewerProcess::getConfigName() );

    EXPECT_TRUE( OCIOViewerProcess::isBuiltinConfig() );

    std::vector<std::string> displays = OCIOViewerProcess::getDisplays();
    EXPECT_NE( displays.end(), std::find(displays.begin(), displays.end(), kDisplay) );
    EXPECT_EQ( kDisplay, OCIOViewerProcess::getDefaultDisplay() );

    std::vector<std::string> views = OCIOViewerProcess::getViews(kDisplay);
    EXPECT_NE( views.end(), std::find(views.begin(), views.end(), kView) );
    EXPECT_EQ( kView, OCIOViewerProcess::getDefaultView(kDisplay) );
}

TEST(OCIOViewerProcess, CPUMatchesOCIO)
{
    ScopedOCIOEnv env(kACES2Studio);
    std::string error;
    OCIOViewerProcessorPtr proc = OCIOViewerProcess::getProcessor(kDisplay, kView, &error);
    ASSERT_TRUE(proc != nullptr) << error;
    // processors are cached per display/view
    EXPECT_EQ( proc, OCIOViewerProcess::getProcessor(kDisplay, kView) );
    EXPECT_FALSE( proc->getCacheID().empty() );

    // reference: the same transform built directly with OCIO (scene_linear is ACEScg in this config)
    namespace OCIO = OCIO_NAMESPACE;
    OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromFile(kACES2Studio);
    OCIO::DisplayViewTransformRcPtr t = OCIO::DisplayViewTransform::Create();
    t->setSrc("ACEScg");
    t->setDisplay(kDisplay);
    t->setView(kView);
    OCIO::ConstCPUProcessorRcPtr ref = config->getProcessor(t)->getDefaultCPUProcessor();

    // black, a dark and a bright gray, a saturated color, and an HDR value, with varied alpha
    float pixels[5 * 4] = {
        0.f,   0.f,  0.f,   1.f,
        0.15f, 0.15f, 0.15f, 1.f,
        0.6f,  0.6f, 0.6f,  0.5f,
        0.8f,  0.1f, 0.02f, 1.f,
        8.f,   4.f,  2.f,   0.f,
    };
    float expected[5 * 4];
    std::copy(pixels, pixels + 20, expected);
    for (int i = 0; i < 5; ++i) {
        ref->applyRGB(&expected[i * 4]);
    }

    proc->applyRGBA(pixels, 5);
    for (int i = 0; i < 20; ++i) {
        EXPECT_NEAR(expected[i], pixels[i], 1e-5) << "component " << i;
    }
    // the display transform is not an identity, and it leaves alpha alone
    EXPECT_GT(pixels[4], 0.15f);
    EXPECT_EQ(0.5f, pixels[11]);
    EXPECT_EQ(0.f, pixels[19]);
}

TEST(OCIOViewerProcess, GPUShader)
{
    ScopedOCIOEnv env(kACES2Studio);
    OCIOViewerProcessorPtr proc = OCIOViewerProcess::getProcessor(kDisplay, kView);
    ASSERT_TRUE(proc != nullptr);
    const OCIOViewerShader& shader = proc->getShader();
    EXPECT_EQ( std::string("OCIODisplay"), shader.functionName );
    EXPECT_NE( std::string::npos, shader.shaderText.find("vec4 OCIODisplay(") );
    // ACES 2.0 output transform: analytic, plus small 1D tables (as Nx1 2D textures)
    ASSERT_FALSE( shader.textures.empty() );
    for (std::size_t i = 0; i < shader.textures.size(); ++i) {
        const OCIOViewerShader::Texture& tex = shader.textures[i];
        EXPECT_NE( std::string::npos, shader.shaderText.find(tex.samplerName) ) << tex.samplerName;
        EXPECT_EQ(0u, tex.depth);
        EXPECT_EQ(1u, tex.height);
        EXPECT_EQ( (std::size_t)tex.width * tex.height * tex.channels, tex.values.size() );
    }
}

#endif // OCIO_VERSION_HEX >= 0x02050000

TEST(OCIOViewerProcess, BadConfigAndView)
{
    {
        ScopedOCIOEnv env("/nonexistent/config.ocio");
        std::string error;
        EXPECT_FALSE( OCIOViewerProcess::loadCurrentConfig(&error) );
        EXPECT_FALSE( error.empty() );
        EXPECT_FALSE( OCIOViewerProcess::isBuiltinConfig() );
        EXPECT_TRUE( OCIOViewerProcess::getDisplays().empty() );
        EXPECT_FALSE( bool( OCIOViewerProcess::getProcessor(kDisplay, kView) ) );
    }
#if OCIO_VERSION_HEX >= 0x02050000
    {
        ScopedOCIOEnv env(kACES2Studio);
        std::string error;
        EXPECT_FALSE( bool( OCIOViewerProcess::getProcessor(kDisplay, "No Such View", &error) ) );
        EXPECT_FALSE( error.empty() );
    }
#endif
}

#else // !NATRON_USE_OCIO

TEST(OCIOViewerProcess, NotAvailable)
{
    EXPECT_FALSE( OCIOViewerProcess::isAvailable() );
    EXPECT_FALSE( OCIOViewerProcess::isBuiltinConfig() );
    EXPECT_TRUE( OCIOViewerProcess::getDisplays().empty() );
    EXPECT_FALSE( bool( OCIOViewerProcess::getProcessor("sRGB", "Raw") ) );
}

#endif // NATRON_USE_OCIO
