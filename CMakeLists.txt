cmake_minimum_required(VERSION 3.25)

project(SpiderlessAzerothSurvivors VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Spiderless Azeroth Survivors requires Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build the x64 configuration for AzerothSurvivors.exe.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Use the MSVC compiler supplied with Visual Studio 2022 or newer.")
endif()

enable_language(RC)
set(CRAB_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/crab")
configure_file(resources/crab_assets.rc.in "${CMAKE_CURRENT_BINARY_DIR}/crab_assets.rc" @ONLY)

# Ship self-contained binaries without requiring a separate VC++ runtime install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(SpiderlessAzerothSurvivors SHARED
    src/dllmain.cpp
    src/avrt_proxy.cpp
    src/avrt_proxy.def
    src/embedded_assets.cpp
    src/patch.cpp
    src/policy.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/crab_assets.rc"
)
target_link_libraries(SpiderlessAzerothSurvivors PRIVATE bcrypt user32)
set_target_properties(SpiderlessAzerothSurvivors PROPERTIES OUTPUT_NAME avrt)

foreach(target IN ITEMS SpiderlessAzerothSurvivors)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(${target} PRIVATE src include)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc>"
    )
endforeach()

install(TARGETS SpiderlessAzerothSurvivors
    RUNTIME DESTINATION .
)
install(FILES README.md DESTINATION .)
install(DIRECTORY docs DESTINATION .)
install(FILES assets/crab/NOTICE.md assets/crab/manifest.json DESTINATION assets)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    install(FILES LICENSE DESTINATION .)
endif()
