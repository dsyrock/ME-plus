target_sources(me_plus PRIVATE
  registry.cpp
  audio_regions/plugin.cpp
)
if(WIN32)
  target_sources(me_plus PRIVATE waveform_thumbnails/plugin.cpp
    native_library_cache/plugin.cpp native_library_cache/disk_cache.cpp)
  target_link_libraries(me_plus PRIVATE bcrypt cabinet)
elseif(APPLE)
  target_sources(me_plus PRIVATE
    audio_regions/mac_preview.mm
    waveform_thumbnails/mac_plugin.mm
  )
else()
  target_sources(me_plus PRIVATE waveform_thumbnails/linux_plugin.cpp)
endif()
