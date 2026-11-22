#include "feature.h"

namespace meplus {

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
};

} // namespace meplus
