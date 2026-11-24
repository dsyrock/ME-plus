cmake_minimum_required(VERSION 3.24)
project(NativeLibraryCache LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The native library cache prototype requires Windows x64 and MSVC.")
endif()

add_library(native_library_cache MODULE plugin.cpp)
set_target_properties(native_library_cache PROPERTIES
  PREFIX "" OUTPUT_NAME reaper_MEPlusNativeCache
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(native_library_cache SYSTEM PRIVATE ../../third_party/reaper)
target_compile_definitions(native_library_cache PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(native_library_cache PRIVATE /W4 /utf-8)
target_link_libraries(native_library_cache PRIVATE bcrypt)

install(TARGETS native_library_cache LIBRARY DESTINATION UserPlugins RUNTIME DESTINATION UserPlugins)
