cmake_minimum_required(VERSION 3.24)

project(obs-beauty VERSION 0.3.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "")

# Face tracking backend: Apple Vision on macOS, ONNX Runtime elsewhere (also usable on macOS).
if(APPLE)
  set(BEAUTY_TRACKER "vision" CACHE STRING "Face tracker backend: vision or onnx")
else()
  set(BEAUTY_TRACKER "onnx" CACHE STRING "Face tracker backend: onnx")
endif()

set(OBS_VERSION "33.0.0" CACHE STRING "OBS version to build against")
set(QT_DEPS_VERSION "2026-10-23" CACHE STRING "obs-deps release providing Qt (must match OBS's Qt)")
set(ORT_VERSION_MAC "1.31.1" CACHE STRING "ONNX Runtime for macOS")
set(ORT_VERSION_WIN "1.24.4" CACHE STRING "ONNX Runtime DirectML for Windows (last DirectML build)")
set(DIRECTML_VERSION "1.15.4" CACHE STRING "DirectML redistributable")
set(DEPS "${CMAKE_SOURCE_DIR}/.deps")
file(MAKE_DIRECTORY "${DEPS}")

function(fetch url archive dest marker)
  if(NOT EXISTS "${marker}")
    message(STATUS "Downloading ${url}")
    file(DOWNLOAD "${url}" "${DEPS}/${archive}" STATUS st)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "Download failed: ${url} (${st})")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${DEPS}/${archive}" DESTINATION "${dest}" ${ARGN})
  endif()
endfunction()

# ---------------------------------------------------------------- headers (all platforms)
set(OBS_SRC "${DEPS}/obs-studio-${OBS_VERSION}")
fetch("https://github.com/obsproject/obs-studio/archive/refs/tags/${OBS_VERSION}.tar.gz" "obs-${OBS_VERSION}.tar.gz"
      "${DEPS}" "${OBS_SRC}/deps/w32-pthreads/pthread.h"
      PATTERNS "obs-studio-${OBS_VERSION}/libobs" "obs-studio-${OBS_VERSION}/deps/w32-pthreads")
set(FRONTEND_DIR "${DEPS}/obs-frontend")
if(NOT EXISTS "${FRONTEND_DIR}/obs-frontend-api.h")
  file(DOWNLOAD "https://raw.githubusercontent.com/obsproject/obs-studio/${OBS_VERSION}/frontend/api/obs-frontend-api.h"
       "${FRONTEND_DIR}/obs-frontend-api.h")
endif()
set(OBS_GEN_DIR "${CMAKE_BINARY_DIR}/obs-generated")
file(WRITE "${OBS_GEN_DIR}/obsconfig.h" "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")

if(APPLE)
  set(SIMDE_DIR "${DEPS}/simde-0.8.2") # libobs headers use SIMDe for SSE on ARM
  fetch("https://github.com/simd-everywhere/simde/archive/refs/tags/v0.8.2.tar.gz" "simde.tar.gz" "${DEPS}"
        "${SIMDE_DIR}/simde/x86/sse2.h" PATTERNS "simde-0.8.2/simde")
endif()

# Qt: OBS's own prebuilt Qt package (same version as inside OBS)
set(QT_DIR "${DEPS}/qt6")
if(APPLE)
  fetch("https://github.com/obsproject/obs-deps/releases/download/${QT_DEPS_VERSION}/macos-deps-qt6-${QT_DEPS_VERSION}-arm64.tar.xz"
        "qt6.tar.xz" "${QT_DIR}" "${QT_DIR}/lib/QtWidgets.framework/Headers/QWidget")
elseif(WIN32)
  fetch("https://github.com/obsproject/obs-deps/releases/download/${QT_DEPS_VERSION}/windows-deps-qt6-${QT_DEPS_VERSION}-x64.zip"
        "qt6.zip" "${QT_DIR}" "${QT_DIR}/include/QtWidgets/QWidget")
endif()

# ---------------------------------------------------------------- ONNX Runtime + models
# Skin segmentation runs on ONNX Runtime on every platform (Core ML on macOS, DirectML on
# Windows); the face landmarks use it too unless BEAUTY_TRACKER=vision.
if(TRUE)
  if(APPLE)
    set(ORT_DIR "${DEPS}/onnxruntime-osx-arm64-${ORT_VERSION_MAC}")
    fetch("https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION_MAC}/onnxruntime-osx-arm64-${ORT_VERSION_MAC}.tgz"
          "ort-mac.tgz" "${DEPS}" "${ORT_DIR}/include/onnxruntime_cxx_api.h")
    set(ORT_INCLUDE "${ORT_DIR}/include")
    file(GLOB ORT_RUNTIME "${ORT_DIR}/lib/libonnxruntime.*.*.*.dylib")
  elseif(WIN32)
    set(ORT_DIR "${DEPS}/ort-dml-${ORT_VERSION_WIN}")
    fetch("https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/${ORT_VERSION_WIN}" "ort-dml.zip"
          "${ORT_DIR}" "${ORT_DIR}/build/native/include/onnxruntime_cxx_api.h")
    set(DML_DIR "${DEPS}/directml-${DIRECTML_VERSION}")
    fetch("https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/${DIRECTML_VERSION}" "directml.zip" "${DML_DIR}"
          "${DML_DIR}/bin/x64-win/DirectML.dll")
    set(ORT_INCLUDE "${ORT_DIR}/build/native/include")
    set(ORT_RUNTIME "${ORT_DIR}/runtimes/win-x64/native/onnxruntime.dll")
  endif()
  # Models (Apache-2.0 / MIT, see data/models/README.md)
  set(MODEL_DIR "${CMAKE_SOURCE_DIR}/data/models")
  foreach(m
      "face_detection_yunet.onnx|https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
      "face_landmarks_478.onnx|https://huggingface.co/senty-au/face_landmarks_detector-ONNX/resolve/main/onnx/model.onnx"
      "selfie_multiclass_256.onnx|https://huggingface.co/senty-au/selfie_multiclass_256x256-ONNX/resolve/main/onnx/model.onnx"
      "selfie_segmentation_landscape.onnx|https://huggingface.co/onnx-community/mediapipe_selfie_segmentation_landscape/resolve/main/onnx/model.onnx")
    string(REPLACE "|" ";" parts "${m}")
    list(GET parts 0 name)
    list(GET parts 1 url)
    if(NOT EXISTS "${MODEL_DIR}/${name}")
      message(STATUS "Downloading model ${name}")
      file(DOWNLOAD "${url}" "${MODEL_DIR}/${name}")
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------- the plugin
add_library(obs-beauty MODULE
  src/plugin-main.c
  src/beauty-filter.c
  src/face-geometry.c
  src/makeup.c
  src/grade.c
  src/presets.c
  src/fx-layers.c
  src/tracker/tracker-core.cpp
  src/tracker/onnx-common.cpp
  src/tracker/segmenter.cpp
  src/tracker/pigment-calib.cpp
  src/dock/beauty-dock.cpp
  src/dock/widgets.cpp
  src/dock/thumbs.cpp
)
if(BEAUTY_TRACKER STREQUAL "vision")
  target_sources(obs-beauty PRIVATE src/tracker/backend-vision.mm)
  target_compile_definitions(obs-beauty PRIVATE BEAUTY_TRACKER_VISION)
  set_source_files_properties(src/tracker/backend-vision.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
  target_sources(obs-beauty PRIVATE src/tracker/backend-onnx.cpp)
endif()
target_include_directories(obs-beauty PRIVATE "${ORT_INCLUDE}")

target_include_directories(obs-beauty PRIVATE "${OBS_SRC}/libobs" "${OBS_GEN_DIR}" "${FRONTEND_DIR}" src src/tracker)
target_compile_definitions(obs-beauty PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}" BEAUTY_HAVE_LIBOBS)

if(MSVC)
  target_compile_options(obs-beauty PRIVATE /utf-8 /W3 /wd4244 /wd4305 /wd4267 /permissive- $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
  target_compile_definitions(obs-beauty PRIVATE _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(obs-beauty PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------- platform linking
if(APPLE)
  set(OBS_APP "/Applications/OBS.app" CACHE PATH "Installed OBS.app to link against")
  target_include_directories(obs-beauty PRIVATE "${SIMDE_DIR}")
  target_compile_options(obs-beauty PRIVATE "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:SHELL:-iframework ${QT_DIR}/lib>")
  target_link_options(obs-beauty PRIVATE "-F${OBS_APP}/Contents/Frameworks" "-Wl,-rpath,@executable_path/../Frameworks")
  target_link_libraries(obs-beauty PRIVATE
    "-framework libobs" "${OBS_APP}/Contents/Frameworks/libobs-frontend-api.1.dylib"
    "-framework QtWidgets" "-framework QtGui" "-framework QtCore"
    "-framework Foundation" "-framework CoreVideo" "-framework CoreGraphics" "-framework Vision")
elseif(WIN32)
  # OBS doesn't ship import libraries, so make them from the official release's DLLs
  # (all of these export plain C symbols).
  set(OBS_BIN "${DEPS}/obs-${OBS_VERSION}-win/bin/64bit")
  fetch("https://github.com/obsproject/obs-studio/releases/download/${OBS_VERSION}/OBS-Studio-${OBS_VERSION}-Windows-x64.zip"
        "obs-win.zip" "${DEPS}/obs-${OBS_VERSION}-win" "${OBS_BIN}/obs.dll")
  get_filename_component(MSVC_BIN "${CMAKE_LINKER}" DIRECTORY)
  find_program(DUMPBIN dumpbin HINTS "${MSVC_BIN}" REQUIRED)
  find_program(LIBEXE lib HINTS "${MSVC_BIN}" REQUIRED)
  set(IMPLIB_DIR "${CMAKE_BINARY_DIR}/implib")
  file(MAKE_DIRECTORY "${IMPLIB_DIR}")
  foreach(dll obs obs-frontend-api w32-pthreads)
    if(NOT EXISTS "${IMPLIB_DIR}/${dll}.lib")
      execute_process(COMMAND "${DUMPBIN}" /NOLOGO /EXPORTS "${OBS_BIN}/${dll}.dll" OUTPUT_VARIABLE exports)
      string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-F]+ +[0-9A-F]+ +[A-Za-z_][A-Za-z0-9_@?$]*" lines "${exports}")
      set(def "LIBRARY ${dll}\nEXPORTS\n")
      foreach(l IN LISTS lines)
        string(REGEX REPLACE ".* ([A-Za-z_][A-Za-z0-9_@?$]*)$" "\\1" sym "${l}")
        string(APPEND def "  ${sym}\n")
      endforeach()
      file(WRITE "${IMPLIB_DIR}/${dll}.def" "${def}")
      execute_process(COMMAND "${LIBEXE}" /NOLOGO /MACHINE:X64 "/DEF:${IMPLIB_DIR}/${dll}.def"
                              "/OUT:${IMPLIB_DIR}/${dll}.lib" RESULT_VARIABLE r)
      if(NOT r EQUAL 0)
        message(FATAL_ERROR "could not create import library for ${dll}.dll")
      endif()
    endif()
  endforeach()
  target_include_directories(obs-beauty PRIVATE "${OBS_SRC}/deps/w32-pthreads" "${QT_DIR}/include"
                             "${QT_DIR}/include/QtCore" "${QT_DIR}/include/QtGui" "${QT_DIR}/include/QtWidgets")
  target_compile_definitions(obs-beauty PRIVATE QT_NO_DEBUG)
  target_link_libraries(obs-beauty PRIVATE
    "${IMPLIB_DIR}/obs.lib" "${IMPLIB_DIR}/obs-frontend-api.lib" "${IMPLIB_DIR}/w32-pthreads.lib"
    "${QT_DIR}/lib/Qt6Widgets.lib" "${QT_DIR}/lib/Qt6Gui.lib" "${QT_DIR}/lib/Qt6Core.lib")
  set_target_properties(obs-beauty PROPERTIES PREFIX "" MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# ---------------------------------------------------------------- data + packaging
file(GLOB_RECURSE PLUGIN_DATA RELATIVE "${CMAKE_SOURCE_DIR}/data" CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/*")
if(NOT BEAUTY_TRACKER STREQUAL "onnx")
  list(FILTER PLUGIN_DATA EXCLUDE REGEX "^models/face_") # Vision does the landmarks itself
endif()
list(FILTER PLUGIN_DATA EXCLUDE REGEX "README\\.md$")

if(APPLE)
  foreach(f IN LISTS PLUGIN_DATA)
    get_filename_component(dir "${f}" DIRECTORY)
    target_sources(obs-beauty PRIVATE "data/${f}")
    set_source_files_properties("data/${f}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${dir}")
  endforeach()
  target_sources(obs-beauty PRIVATE "${ORT_RUNTIME}")
  set_source_files_properties("${ORT_RUNTIME}" PROPERTIES MACOSX_PACKAGE_LOCATION "Frameworks")
  get_filename_component(ORT_NAME "${ORT_RUNTIME}" NAME)
  target_compile_definitions(obs-beauty PRIVATE BEAUTY_ORT_FILE="${ORT_NAME}")
  set_target_properties(obs-beauty PROPERTIES
    BUNDLE TRUE BUNDLE_EXTENSION plugin PREFIX ""
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "id.tunelab.obs-beauty"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
  set(OBS_USER_PLUGINS "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
  add_custom_target(install-plugin
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${OBS_USER_PLUGINS}/obs-beauty.plugin"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:obs-beauty>" "${OBS_USER_PLUGINS}/obs-beauty.plugin"
    DEPENDS obs-beauty
    COMMENT "Installing obs-beauty.plugin into ${OBS_USER_PLUGINS}")
elseif(WIN32)
  # Layout matches an OBS install: obs-plugins/64bit/*.dll + data/obs-plugins/obs-beauty/*
  target_compile_definitions(obs-beauty PRIVATE BEAUTY_ORT_FILE="obs-beauty-onnxruntime.dll")
  install(TARGETS obs-beauty LIBRARY DESTINATION obs-plugins/64bit RUNTIME DESTINATION obs-plugins/64bit)
  install(FILES "${ORT_RUNTIME}" DESTINATION obs-plugins/64bit RENAME obs-beauty-onnxruntime.dll)
  install(FILES "${DML_DIR}/bin/x64-win/DirectML.dll" DESTINATION obs-plugins/64bit)
  foreach(f IN LISTS PLUGIN_DATA)
    get_filename_component(dir "${f}" DIRECTORY)
    install(FILES "data/${f}" DESTINATION "data/obs-plugins/obs-beauty/${dir}")
  endforeach()
  set(CPACK_GENERATOR ZIP)
  set(CPACK_PACKAGE_FILE_NAME "obs-beauty-${PROJECT_VERSION}-windows-x64")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
  include(CPack)
endif()
