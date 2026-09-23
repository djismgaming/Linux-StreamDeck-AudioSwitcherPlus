include(FetchContent)

if(UNIX AND NOT APPLE)
  # Upstream AudioDeviceLib only ships Windows and macOS backends; on Linux we
  # build our own libpulse-based implementation of the same API (see
  # Sources/AudioDevicesLinux/AudioDevicesLinux.cpp).
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBPULSE REQUIRED IMPORTED_TARGET libpulse)
  add_library(AudioDeviceLib STATIC)
  target_sources(AudioDeviceLib PRIVATE Sources/AudioDevicesLinux/AudioDevicesLinux.cpp)
  target_include_directories(
    AudioDeviceLib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Sources/AudioDevicesLinux/include"
  )
  target_compile_features(AudioDeviceLib PUBLIC cxx_std_20)
  target_link_libraries(AudioDeviceLib PUBLIC PkgConfig::LIBPULSE)
  return()
endif()

FetchContent_Declare(
  AudioDeviceLib
  GIT_REPOSITORY https://github.com/fredemmott/AudioDeviceLib
  GIT_TAG 8cef359dd7920659a24aa1aabf9539fe2dffb01b
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)

FetchContent_GetProperties(AudioDeviceLib)
if(NOT audiodevicelib_POPULATED)
  FetchContent_Populate(AudioDeviceLib)
  add_subdirectory("${audiodevicelib_SOURCE_DIR}" "${audiodevicelib_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()
