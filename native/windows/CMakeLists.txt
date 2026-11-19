cmake_minimum_required(VERSION 3.24)
project(EvpRemote LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Use a Windows x64 MSVC toolchain.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets WebSockets)
set(EVP_WEBRTC_ROOT "" CACHE PATH "Root of the x64 libwebrtc SDK")
set(EVP_FFMPEG_ROOT "" CACHE PATH "Root of the shared x64 FFmpeg development distribution")
find_path(WEBRTC_INCLUDE webrtc_c.h PATHS "${EVP_WEBRTC_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
find_library(WEBRTC_LIBRARY NAMES webrtc_c PATHS "${EVP_WEBRTC_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_path(FFMPEG_INCLUDE libavcodec/avcodec.h PATHS "${EVP_FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
foreach(component avcodec avutil swscale swresample)
    find_library(FFMPEG_${component} NAMES ${component} PATHS "${EVP_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()
add_library(evp_client STATIC
    controller.cpp controller.h code_options.h viewer_input.cpp viewer_input.h
    controller_dispatch.h controller_host.cpp host_input.cpp host_input.h
    devices/device_connection.cpp devices/device_connection.h devices/device_identity.cpp devices/device_identity.h
    ui/home_window.cpp ui/home_window.h ui/session_window.cpp ui/session_window.h
    ui/device_page.cpp ui/device_page.h
    media/video_surface.cpp media/video_surface.h media/ffmpeg_decoder.cpp media/ffmpeg_decoder.h
    media/webrtc_ffmpeg_decoder.cpp media/webrtc_ffmpeg_decoder.h
    rtc/rtc_viewer.cpp rtc/rtc_viewer.h rtc/rtc_host.cpp rtc/rtc_host.h
    capture/desktop_capture.cpp capture/desktop_capture.h capture/loopback_audio.cpp capture/loopback_audio.h
    capture/webrtc_ffmpeg_encoder.cpp capture/webrtc_ffmpeg_encoder.h)
target_include_directories(evp_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${WEBRTC_INCLUDE}" "${FFMPEG_INCLUDE}")
target_compile_options(evp_client PRIVATE /utf-8 /EHsc /W3)
target_compile_definitions(evp_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WIN32)
target_link_options(evp_client INTERFACE /NODEFAULTLIB:LIBCMT)
target_link_libraries(evp_client PUBLIC Qt6::Widgets Qt6::WebSockets PRIVATE
    "${WEBRTC_LIBRARY}" "${FFMPEG_avcodec}" "${FFMPEG_avutil}" "${FFMPEG_swscale}" "${FFMPEG_swresample}"
    d3d11 dxgi d3dcompiler user32 ws2_32 winmm avrt uuid strmiids dmoguids iphlpapi msdmo
    secur32 wmcodecdspuuid ole32 oleaut32 advapi32 bcrypt crypt32 ntdll userenv Wtsapi32)
qt_add_executable(EvpRemote WIN32 main.cpp)
target_compile_options(EvpRemote PRIVATE /utf-8 /EHsc)
target_link_libraries(EvpRemote PRIVATE evp_client)

option(EVP_BUILD_PROBES "Build the loopback interop test driver" OFF)
if(EVP_BUILD_PROBES)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(EvpSessionProbe tests/session_probe.cpp)
    target_compile_options(EvpSessionProbe PRIVATE /utf-8 /EHsc)
    target_link_libraries(EvpSessionProbe PRIVATE evp_client Qt6::Test)
    add_executable(EvpUnifiedProbe tests/unified_probe_main.cpp tests/host_session_probe.cpp
                   tests/session_probe.cpp tests/device_security_probe.cpp)
    target_include_directories(EvpUnifiedProbe PRIVATE "${WEBRTC_INCLUDE}")
    target_compile_definitions(EvpUnifiedProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN EVP_UNIFIED_PROBE)
    target_compile_options(EvpUnifiedProbe PRIVATE /utf-8 /EHsc)
    target_link_libraries(EvpUnifiedProbe PRIVATE evp_client Qt6::Test)
    add_executable(EvpHostInputProbe tests/host_input_probe.cpp)
    target_compile_options(EvpHostInputProbe PRIVATE /utf-8 /EHsc)
    target_link_libraries(EvpHostInputProbe PRIVATE evp_client Qt6::Test)
endif()
