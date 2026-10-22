cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)

# LGPL compliance: use the shared MSVC runtime, never a static CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

project(play VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-lgpl"
    CACHE PATH "FFmpeg LGPL shared build root (include/, lib/, bin/)")
set(QT_ROOT "" CACHE PATH "Qt 6 MSVC 2022 64-bit root (bin/, lib/, include/)")

# Qt is deliberately resolved from an external, shared installation. The
# package never ships a static Qt and the build fails if one is selected.
if(NOT QT_ROOT AND DEFINED ENV{QT_ROOT})
    set(QT_ROOT "$ENV{QT_ROOT}")
endif()
if(QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${QT_ROOT}")
endif()

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 ShaderTools)

# Verify that the found Qt is a shared build; static Qt cannot be used for an
# LGPL distribution because it removes the user's ability to replace the libs.
get_target_property(_qt_core_implib Qt6::Core IMPORTED_IMPLIB_RELEASE)
if(NOT _qt_core_implib)
    get_target_property(_qt_core_implib Qt6::Core IMPORTED_IMPLIB)
endif()
get_target_property(_qt_core_type Qt6::Core TYPE)
if(NOT _qt_core_implib AND NOT _qt_core_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR
        "Qt must be a shared (DLL) build. Point QT_ROOT at a Qt MSVC 2022 64-bit "
        "shared installation, for example -DQT_ROOT=C:/Qt/6.8.3/msvc2022_64")
endif()

# Resolve the installation root from the found Qt when QT_ROOT was omitted, so
# the QRhi include path and windeployqt can still be located.
set(_qt_bin_dir "")
get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT _qt_core_loc)
    get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION)
endif()
if(_qt_core_loc)
    get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
    if(NOT QT_ROOT)
        get_filename_component(QT_ROOT "${_qt_bin_dir}" DIRECTORY)
    endif()
endif()

# --- FFmpeg (LGPL, shared, import libraries only) ---------------------------
foreach(lib avformat avcodec avfilter avutil swresample swscale)
    if(NOT EXISTS "${FFMPEG_ROOT}/lib/${lib}.lib")
        message(FATAL_ERROR
            "Missing ${FFMPEG_ROOT}/lib/${lib}.lib. Use the LGPL shared FFmpeg "
            "development package (include/, lib/, bin/) and set FFMPEG_ROOT.")
    endif()
endforeach()

file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
if(NOT FFMPEG_DLLS)
    message(FATAL_ERROR
        "No FFmpeg runtime DLLs found in ${FFMPEG_ROOT}/bin. A shared FFmpeg "
        "build is required; static linking is not supported.")
endif()

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
foreach(lib avformat avcodec avfilter avutil swresample swscale)
    target_link_libraries(ffmpeg INTERFACE "${FFMPEG_ROOT}/lib/${lib}.lib")
endforeach()

# --- Application ------------------------------------------------------------
qt_add_executable(play
    src/main.cpp
    src/SelfTest.cpp
    src/core/VideoDecoder.cpp
    src/core/AudioDecoder.cpp
    src/core/Player.cpp
    src/core/Recorder.cpp
    src/core/Library.cpp
    src/core/Settings.cpp
    src/core/PlaylistModel.cpp
    src/core/SubtitleTrack.cpp
    src/audio/AudioSink.cpp
    src/render/VideoItem.cpp
    src/render/RenderGraph.cpp
    src/render/SamplePass.cpp
    src/render/d3d11/D3D11VideoProcessor.cpp
)

qt_add_resources(play "app_qml"
    PREFIX "/"
    FILES qml/Main.qml
)

qt6_add_shaders(play "video_shaders"
    PREFIX "/"
    FILES
        shaders/video.vert
        shaders/video.frag
        shaders/effect.frag
        shaders/composite.frag
        shaders/scale.frag
        shaders/record.frag
    GLSL "150"
    HLSL 50
    MSL 12
)

target_include_directories(play PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio"
)

target_link_libraries(play PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Gui
    Qt6::Core
    ffmpeg
)

# QRhi public headers live in include/QtGui/<ver>/QtGui/rhi, which Qt does not
# add to the include path for Qt6::Gui.
file(GLOB _qt_gui_rhi "${QT_ROOT}/include/QtGui/*/QtGui/rhi")
if(_qt_gui_rhi)
    get_filename_component(_qt_gui_versioned "${_qt_gui_rhi}" DIRECTORY)
    target_include_directories(play PRIVATE "${_qt_gui_versioned}")
endif()

if(WIN32)
    target_link_libraries(play PRIVATE d3d11)
endif()

# Dynamic FFmpeg runtime next to the executable, plus the LGPL notice and
# license texts that must accompany every distributed build.
add_custom_command(TARGET play POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:play>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES" "$<TARGET_FILE_DIR:play>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-lgpl/LICENSE.txt"
            "$<TARGET_FILE_DIR:play>/licenses/FFmpeg-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md" "$<TARGET_FILE_DIR:play>/NOTICE.md"
    COMMENT "Copying FFmpeg DLLs and LGPL notices"
)

# Release bundle: `cmake --build build --config Release --target deploy`
# windeployqt copies the shared Qt DLLs and QML plugins next to play.exe.
find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin_dir}" "${QT_ROOT}/bin")
if(WINDEPLOYQT_EXE)
    add_custom_target(deploy
        COMMAND "${WINDEPLOYQT_EXE}" --no-translations --compiler-runtime
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml" "$<TARGET_FILE:play>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES" "$<TARGET_FILE_DIR:play>/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-lgpl/LICENSE.txt"
                "$<TARGET_FILE_DIR:play>/licenses/FFmpeg-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md" "$<TARGET_FILE_DIR:play>/NOTICE.md"
        DEPENDS play
        COMMENT "Deploying shared Qt runtime and LGPL notices"
    )
endif()
