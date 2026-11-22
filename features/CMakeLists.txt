target_sources(me_plus PRIVATE
  registry.cpp
  audio_regions/plugin.cpp
)
if(WIN32)
  target_sources(me_plus PRIVATE waveform_thumbnails/plugin.cpp)
elseif(APPLE)
  target_sources(me_plus PRIVATE waveform_thumbnails/mac_plugin.mm)
else()
  target_sources(me_plus PRIVATE waveform_thumbnails/linux_plugin.cpp)
endif()
