cmake_minimum_required(VERSION 3.24)
project(MEPlus LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ME+ requires a 64-bit target.")
endif()

add_library(me_plus MODULE core/plugin.cpp)
set_target_properties(me_plus PROPERTIES PREFIX "" OUTPUT_NAME "reaper_ME+"
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
target_include_directories(me_plus PRIVATE core)
find_package(Threads REQUIRED)
target_link_libraries(me_plus PRIVATE Threads::Threads)

if(WIN32)
  if(NOT MSVC)
    message(FATAL_ERROR "ME+ requires the MSVC ABI on Windows.")
  endif()
  target_include_directories(me_plus SYSTEM PRIVATE third_party/reaper)
  target_compile_definitions(me_plus PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_compile_options(me_plus PRIVATE /utf-8 /W4)
  set_property(TARGET me_plus PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_libraries(me_plus PRIVATE comctl32 gdi32 user32)
elseif(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(FetchContent)
  FetchContent_Declare(wdl
    GIT_REPOSITORY https://github.com/justinfrankel/WDL.git
    GIT_TAG 03c6e564b881a41cb13441cfd7350e92ae1ddf4d
    SOURCE_SUBDIR _headers_only)
  FetchContent_MakeAvailable(wdl)

  set(sdk_dir "${CMAKE_CURRENT_BINARY_DIR}/sdk")
  foreach(header reaper_plugin.h reaper_plugin_functions.h)
    configure_file("third_party/reaper/${header}" "${sdk_dir}/reaper/${header}" COPYONLY)
  endforeach()
  file(CREATE_LINK "${wdl_SOURCE_DIR}/WDL" "${sdk_dir}/WDL" SYMBOLIC)
  target_include_directories(me_plus SYSTEM PRIVATE "${sdk_dir}/reaper" "${sdk_dir}")
  target_compile_definitions(me_plus PRIVATE SWELL_PROVIDED_BY_APP NOMINMAX)

  if(APPLE)
    enable_language(OBJCXX)
    set_target_properties(me_plus PROPERTIES SUFFIX ".dylib"
      OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_sources(me_plus PRIVATE "${wdl_SOURCE_DIR}/WDL/swell/swell-modstub.mm")
    target_compile_options(me_plus PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fno-objc-arc>)
    target_link_libraries(me_plus PRIVATE "-framework Cocoa")
  else()
    target_sources(me_plus PRIVATE "${wdl_SOURCE_DIR}/WDL/swell/swell-modstub-generic.cpp")
    target_compile_options(me_plus PRIVATE -include cstddef)
  endif()
else()
  message(FATAL_ERROR "Unsupported operating system for ME+.")
endif()

add_subdirectory(features)
install(TARGETS me_plus LIBRARY DESTINATION . RUNTIME DESTINATION .)
