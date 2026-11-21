#pragma once

#include <initializer_list>
#include <reaper_plugin.h>

namespace meplus {

struct Feature {
    bool (*load)(reaper_plugin_info_t& host) noexcept;
    void (*unload)(reaper_plugin_info_t& host) noexcept;
};

extern const std::initializer_list<Feature> features;

} // namespace meplus
