#include "feature.h"

namespace {

reaper_plugin_info_t host{};
std::size_t loaded_count = 0;

void unload_features() noexcept
{
    while (loaded_count > 0) {
        --loaded_count;
        meplus::features.begin()[loaded_count].unload();
    }
    host = {};
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t* info)
{
    if (!info) {
        unload_features();
        return 0;
    }
    if (host.Register || info->caller_version != REAPER_PLUGIN_VERSION || !info->Register || !info->GetFunc) {
        return 0;
    }

    host = *info;
    for (const auto& feature : meplus::features) {
        if (!feature.load(instance, host)) {
            unload_features();
            return 0;
        }
        ++loaded_count;
    }
    return 1;
}
