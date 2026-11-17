cmake_minimum_required(VERSION 3.25)
project(EchoLab VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

# VST3 SDK (MIT) — plugin target only, no examples, no VSTGUI yet
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
if(NOT LAB_TEST_HOST)  # -DLAB_TEST_HOST=ON builds the SDK editorhost to open the plugin windows outside Live
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
endif()
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
    third_party/vst3sdk/public.sdk/source/common/pluginview.cpp
    plugin/common/WebEditor.mm
)
target_compile_features(EchoLab PRIVATE cxx_std_20)
target_include_directories(EchoLab PRIVATE plugin/source dsp)
target_link_libraries(EchoLab PRIVATE sdk)
smtg_target_configure_version_file(EchoLab)
target_include_directories(EchoLab PRIVATE plugin/common)
if(SMTG_MAC)
    # the face: a WKWebView showing ui/prototype/faces.html (bundled into Contents/Resources)
    target_link_libraries(EchoLab PRIVATE "-framework WebKit" "-framework Cocoa")
    target_compile_options(EchoLab PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(EchoLab PRIVATE LAB_OBJC_PREFIX=EchoLabFace)
    smtg_target_add_plugin_resources(EchoLab RESOURCES ui/prototype/faces.html)
endif()
if(SMTG_MAC)
    smtg_target_set_bundle(EchoLab
        BUNDLE_IDENTIFIER "local.echolab.echo"
        COMPANY_NAME "Echo Lab")
endif()

# offline harness (same engine, for measurement)
add_executable(render harness/render.cpp)
target_compile_features(render PRIVATE cxx_std_20)

# ---------------------------------------------------------------- Drive Lab (saturation + pump + filter)
smtg_add_vst3plugin(DriveLab
    third_party/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    plugin/drive/plugin.cpp
    plugin/drive/entry.cpp
    third_party/vst3sdk/public.sdk/source/common/pluginview.cpp
    plugin/common/WebEditor.mm
)
target_compile_features(DriveLab PRIVATE cxx_std_20)
target_include_directories(DriveLab PRIVATE plugin/drive dsp)
target_link_libraries(DriveLab PRIVATE sdk)
smtg_target_configure_version_file(DriveLab)
target_include_directories(DriveLab PRIVATE plugin/common)
if(SMTG_MAC)
    # the face: a WKWebView showing ui/prototype/faces.html (bundled into Contents/Resources)
    target_link_libraries(DriveLab PRIVATE "-framework WebKit" "-framework Cocoa")
    target_compile_options(DriveLab PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(DriveLab PRIVATE LAB_OBJC_PREFIX=DriveLabFace)
    smtg_target_add_plugin_resources(DriveLab RESOURCES ui/prototype/faces.html)
endif()
if(SMTG_MAC)
    smtg_target_set_bundle(DriveLab
        BUNDLE_IDENTIFIER "local.echolab.drive"
        COMPANY_NAME "Echo Lab")
endif()

# ---------------------------------------------------------------- Lethe (reverb)
smtg_add_vst3plugin(Lethe
    third_party/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    plugin/reverb/plugin.cpp
    plugin/reverb/entry.cpp
    third_party/vst3sdk/public.sdk/source/common/pluginview.cpp
    plugin/common/WebEditor.mm
)
target_compile_features(Lethe PRIVATE cxx_std_20)
target_include_directories(Lethe PRIVATE plugin/reverb dsp)
target_link_libraries(Lethe PRIVATE sdk)
smtg_target_configure_version_file(Lethe)
target_include_directories(Lethe PRIVATE plugin/common)
if(SMTG_MAC)
    target_link_libraries(Lethe PRIVATE "-framework WebKit" "-framework Cocoa")
    target_compile_options(Lethe PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(Lethe PRIVATE LAB_OBJC_PREFIX=LetheFace)
    smtg_target_add_plugin_resources(Lethe RESOURCES ui/prototype/faces.html)
    smtg_target_set_bundle(Lethe
        BUNDLE_IDENTIFIER "local.echolab.lethe"
        COMPANY_NAME "Echo Lab")
endif()
add_executable(render_verb harness/render_verb.cpp)
target_compile_features(render_verb PRIVATE cxx_std_20)

# ---------------------------------------------------------------- Charon (pump / duck, sidechain)
smtg_add_vst3plugin(Charon
    third_party/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    plugin/pump/plugin.cpp
    plugin/pump/entry.cpp
    third_party/vst3sdk/public.sdk/source/common/pluginview.cpp
    plugin/common/WebEditor.mm
)
target_compile_features(Charon PRIVATE cxx_std_20)
target_include_directories(Charon PRIVATE plugin/pump dsp plugin/common)
target_link_libraries(Charon PRIVATE sdk)
smtg_target_configure_version_file(Charon)
if(SMTG_MAC)
    target_link_libraries(Charon PRIVATE "-framework WebKit" "-framework Cocoa")
    target_compile_options(Charon PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(Charon PRIVATE LAB_OBJC_PREFIX=CharonFace)
    smtg_target_add_plugin_resources(Charon RESOURCES ui/prototype/faces.html)
    smtg_target_set_bundle(Charon
        BUNDLE_IDENTIFIER "local.echolab.charon"
        COMPANY_NAME "Echo Lab")
endif()

# ---------------------------------------------------------------- Morpheus (wavetable synth)
smtg_add_vst3plugin(Morpheus
    third_party/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    plugin/synth/plugin.cpp
    plugin/synth/entry.cpp
    third_party/vst3sdk/public.sdk/source/common/pluginview.cpp
    plugin/common/WebEditor.mm
)
target_compile_features(Morpheus PRIVATE cxx_std_20)
target_include_directories(Morpheus PRIVATE plugin/synth dsp plugin/common)
target_link_libraries(Morpheus PRIVATE sdk)
smtg_target_configure_version_file(Morpheus)
if(SMTG_MAC)
    target_link_libraries(Morpheus PRIVATE "-framework WebKit" "-framework Cocoa")
    target_compile_options(Morpheus PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(Morpheus PRIVATE LAB_OBJC_PREFIX=MorpheusFace)
    smtg_target_add_plugin_resources(Morpheus RESOURCES ui/prototype/morpheus.html)
    smtg_target_set_bundle(Morpheus
        BUNDLE_IDENTIFIER "local.echolab.morpheus"
        COMPANY_NAME "Echo Lab")
endif()

# ---------------------------------------------------------------- host-parameter contract tests
# (Live clamps plain values into [plain(0), plain(1)]; see harness/test_params.cpp)
enable_testing()
foreach(t "echo;source;echolab" "drive;drive;echolab::drive" "verb;reverb;echolab::reverb" "pump;pump;echolab::pumpfx" "synth;synth;echolab::wtsynth")
    list(GET t 0 name)
    list(GET t 1 dir)
    list(GET t 2 ns)
    add_executable(test_params_${name} harness/test_params.cpp)
    target_compile_features(test_params_${name} PRIVATE cxx_std_20)
    target_compile_definitions(test_params_${name} PRIVATE
        "PARAMS_H=\"${CMAKE_SOURCE_DIR}/plugin/${dir}/params.h\"" PLUGIN_NS=${ns} "PLUGIN_LABEL=\"${name}\"")
    add_test(NAME params_${name} COMMAND test_params_${name})
endforeach()

add_executable(test_synth harness/test_synth.cpp)
target_compile_features(test_synth PRIVATE cxx_std_20)
target_include_directories(test_synth PRIVATE dsp)
add_test(NAME synth_engine COMMAND test_synth ${CMAKE_BINARY_DIR}/synth_demo.wav)
add_executable(test_resynth harness/test_resynth.cpp)
target_compile_features(test_resynth PRIVATE cxx_std_20)
target_include_directories(test_resynth PRIVATE dsp)
add_test(NAME synth_resynth COMMAND test_resynth)
add_executable(test_filters harness/test_filters.cpp)
target_compile_features(test_filters PRIVATE cxx_std_20)
target_include_directories(test_filters PRIVATE dsp)
add_test(NAME synth_filters COMMAND test_filters)
add_executable(test_mod harness/test_mod.cpp)
target_compile_features(test_mod PRIVATE cxx_std_20)
target_include_directories(test_mod PRIVATE dsp)
add_test(NAME synth_mod COMMAND test_mod)
add_executable(test_osc harness/test_osc.cpp)
target_compile_features(test_osc PRIVATE cxx_std_20)
target_include_directories(test_osc PRIVATE dsp)
add_test(NAME synth_osc COMMAND test_osc)
