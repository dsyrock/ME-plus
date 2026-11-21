#include "feature.h"

namespace {

reaper_plugin_info_t* host = nullptr;
std::size_t loaded_count = 0;

void unload_features() noexcept
{
    while (loaded_count > 0) {
        --loaded_count;
        meplus::features.begin()[loaded_count].unload(*host);
    }
    host = nullptr;
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* info)
{
    if (!info) {
        unload_features();
        return 0;
    }
    if (host || info->caller_version != REAPER_PLUGIN_VERSION || !info->Register || !info->GetFunc) {
        return 0;
    }

    host = info;
    for (const auto& feature : meplus::features) {
        if (!feature.load(*host)) {
            unload_features();
            return 0;
        }
        ++loaded_count;
    }
    return 1;
}
