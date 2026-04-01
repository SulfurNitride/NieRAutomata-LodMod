cmake_minimum_required(VERSION 3.20)
project(Automata-LodMod LANGUAGES CXX C)

set(TARGET_NAME "lodmod")
set(OUTPUT_DLL_NAME "dxgi" CACHE STRING "Output DLL name")

add_library(${TARGET_NAME} SHARED
    Automata-LodMod/pch.cpp
    Automata-LodMod/dllmain.cpp
    Automata-LodMod/proxy.cpp
    Automata-LodMod/Utility.cpp
    Automata-LodMod/AOFixes.cpp
    Automata-LodMod/CriH264.cpp
    Automata-LodMod/CriH264_wrappers.cpp
    Automata-LodMod/CriUSM.cpp
    Automata-LodMod/MapMod.cpp
    Automata-LodMod/Rebug.cpp
    Automata-LodMod/ShadowFixes.cpp
    Automata-LodMod/Translate.cpp
    Automata-LodMod/Translate_FlagDescs.cpp
    Automata-LodMod/FileLimits.cpp
    Automata-LodMod/TextureInjection.cpp
    Automata-LodMod/third_party/MinHook/buffer.c
    Automata-LodMod/third_party/MinHook/hook.c
    Automata-LodMod/third_party/MinHook/trampoline.c
    Automata-LodMod/third_party/MinHook/hde/hde32.c
    Automata-LodMod/third_party/MinHook/hde/hde64.c
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexImage.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexUtil.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexDDS.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexD3D11.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexMisc.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexConvert.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexCompress.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexMipmaps.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexResize.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexFlipRotate.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexNormalMaps.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/DirectXTexPMAlpha.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/BC.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/BC4BC5.cpp
    Automata-LodMod/third_party/DirectXTex/DirectXTex/BC6HBC7.cpp
    Automata-LodMod/proxy.def
)

set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME "${OUTPUT_DLL_NAME}" PREFIX "")

target_include_directories(${TARGET_NAME} PRIVATE
    Automata-LodMod
    Automata-LodMod/third_party/MinHook
    Automata-LodMod/third_party/DirectXTex/DirectXTex
)

target_compile_definitions(${TARGET_NAME} PRIVATE
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    LODMOD_EXPORTS _WINDOWS _USRDLL NOMINMAX
    UNICODE _UNICODE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN
)

target_link_libraries(${TARGET_NAME} PRIVATE
    d3d11 dxgi shlwapi shell32 ole32 uuid psapi Mfplat mfuuid
)

target_compile_options(${TARGET_NAME} PRIVATE
    "SHELL:/imsvc $ENV{HOME}/.xwin/crt/include"
    "SHELL:/imsvc $ENV{HOME}/.xwin/sdk/include/ucrt"
    "SHELL:/imsvc $ENV{HOME}/.xwin/sdk/include/shared"
    "SHELL:/imsvc $ENV{HOME}/.xwin/sdk/include/um"
    "SHELL:/imsvc $ENV{HOME}/.xwin/sdk/include/winrt"
)

set(MOD_OUTPUT_DIR "$ENV{HOME}/Games/Nier/mods/LodModV2/Root" CACHE PATH "")
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MOD_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET_NAME}>" "${MOD_OUTPUT_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LodMod.ini" "${MOD_OUTPUT_DIR}/"
)
