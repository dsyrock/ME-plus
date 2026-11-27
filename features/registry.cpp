#include "feature.h"

namespace meplus {

#ifdef _WIN32
namespace native_library_cache {
bool load(REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t& host);
void unload();
}
#endif

namespace audio_regions {
bool load(REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t& host);
void unload();
}

namespace waveform_thumbnails {
bool load(REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t& host);
void unload();
}

const std::initializer_list<Feature> features = {
    {audio_regions::load, audio_regions::unload},
    {waveform_thumbnails::load, waveform_thumbnails::unload},
#ifdef _WIN32
    {native_library_cache::load, native_library_cache::unload},
#endif
};

} // namespace meplus
