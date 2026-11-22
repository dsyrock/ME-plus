#pragma once

#include <initializer_list>
#include <reaper_plugin.h>

namespace meplus {

struct Feature {
    bool (*load)(REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t& host);
    void (*unload)();
};

extern const std::initializer_list<Feature> features;

} // namespace meplus
