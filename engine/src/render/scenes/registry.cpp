#include "av/render/scene.h"
#include "bars_frag.h"
#include "fractal_frag.h"
#include "ring_frag.h"
#include "tunnel_frag.h"

namespace av {

std::vector<std::unique_ptr<Scene>> createBuiltinScenes() {
    std::vector<std::unique_ptr<Scene>> s;
    s.push_back(std::make_unique<FullscreenScene>("spectrum-bars", shaders::bars_frag, shaders::bars_fragSize));
    s.push_back(std::make_unique<FullscreenScene>("radial-spectrum", shaders::ring_frag, shaders::ring_fragSize));
    s.push_back(std::make_unique<FullscreenScene>("kaleido-tunnel", shaders::tunnel_frag, shaders::tunnel_fragSize));
    s.push_back(std::make_unique<FullscreenScene>("fractal-flight", shaders::fractal_frag, shaders::fractal_fragSize));
    return s;
}

}  // namespace av
