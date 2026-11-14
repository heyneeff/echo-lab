cmake_minimum_required(VERSION 3.25)
project(EchoLab VERSION 0.1.0 LANGUAGES C CXX)

# VST3 SDK (MIT) — plugin target only, no examples, no VSTGUI yet
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR ON CACHE BOOL "" FORCE)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
add_subdirectory(third_party/vst3sdk ${PROJECT_BINARY_DIR}/vst3sdk EXCLUDE_FROM_ALL)
smtg_enable_vst3_sdk()

smtg_add_vst3plugin(EchoLab
    third_party/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    plugin/source/plugin.cpp
    plugin/source/entry.cpp
)
target_compile_features(EchoLab PRIVATE cxx_std_20)
target_include_directories(EchoLab PRIVATE plugin/source dsp)
target_link_libraries(EchoLab PRIVATE sdk)
smtg_target_configure_version_file(EchoLab)
if(SMTG_MAC)
    smtg_target_set_bundle(EchoLab
        BUNDLE_IDENTIFIER "local.echolab.echo"
        COMPANY_NAME "Echo Lab")
endif()

# offline harness (same engine, for measurement)
add_executable(render harness/render.cpp)
target_compile_features(render PRIVATE cxx_std_20)
