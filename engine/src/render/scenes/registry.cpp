#include "av/render/scene.h"
#include "bars_frag.h"

namespace av {

std::vector<std::unique_ptr<Scene>> createBuiltinScenes() {
    std::vector<std::unique_ptr<Scene>> s;
    s.push_back(std::make_unique<FullscreenScene>("spectrum-bars", shaders::bars_frag, shaders::bars_fragSize));
    return s;
}

}  // namespace av
