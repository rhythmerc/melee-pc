# OpenXR presentation (lib/xr): the game's frame goes to a virtual screen in
# the headset. Linux uses the system OpenXR loader (e.g. Monado for desktop
# testing, enabled at runtime with AURORA_XR=1). Android fetches Khronos'
# loader AAR, links it, stages libopenxr_loader.so next to the build output
# for packaging, and turns XR on by default.
include("${CMAKE_CURRENT_LIST_DIR}/AuroraDependencyVersions.cmake")

target_sources(aurora_core PRIVATE lib/xr/xr.cpp)
target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_OPENXR)

if (ANDROID)
  set(_xr_dir "${CMAKE_BINARY_DIR}/_deps/openxr_loader_android-${AURORA_OPENXR_ANDROID_VERSION}")
  set(_xr_aar "${_xr_dir}.aar")
  if (NOT EXISTS "${_xr_dir}/prefab")
    set(_xr_url "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/${AURORA_OPENXR_ANDROID_VERSION}/openxr_loader_for_android-${AURORA_OPENXR_ANDROID_VERSION}.aar")
    message(STATUS "aurora: fetching OpenXR loader ${AURORA_OPENXR_ANDROID_VERSION} for Android")
    file(DOWNLOAD "${_xr_url}" "${_xr_aar}" EXPECTED_HASH SHA256=${AURORA_OPENXR_ANDROID_SHA256} STATUS _xr_status)
    list(GET _xr_status 0 _xr_code)
    if (NOT _xr_code EQUAL 0)
      message(FATAL_ERROR "aurora: downloading ${_xr_url} failed: ${_xr_status}")
    endif ()
    file(ARCHIVE_EXTRACT INPUT "${_xr_aar}" DESTINATION "${_xr_dir}")
  endif ()
  set(_xr_so "${_xr_dir}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so")
  add_library(aurora_openxr_loader SHARED IMPORTED)
  set_target_properties(aurora_openxr_loader PROPERTIES
          IMPORTED_LOCATION "${_xr_so}"
          IMPORTED_NO_SONAME TRUE
          INTERFACE_INCLUDE_DIRECTORIES "${_xr_dir}/prefab/modules/headers/include")
  # tools/build_android.sh copies it into jniLibs beside libmelee.so.
  configure_file("${_xr_so}" "${CMAKE_BINARY_DIR}/libopenxr_loader.so" COPYONLY)
  target_compile_definitions(aurora_core PRIVATE AURORA_XR_DEFAULT_ON)
  target_link_libraries(aurora_core PRIVATE aurora_openxr_loader vulkan nativewindow android)
else ()
  find_package(OpenXR REQUIRED CONFIG)
  find_package(Vulkan REQUIRED)
  target_link_libraries(aurora_core PRIVATE OpenXR::openxr_loader Vulkan::Vulkan)
endif ()
