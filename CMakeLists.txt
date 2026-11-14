cmake_minimum_required(VERSION 3.20)
project(Aether LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN=0)

# Dear ImGui: use third_party/imgui if present, otherwise download it.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.h")
    set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
else()
    include(FetchContent)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

file(GLOB_RECURSE AETHER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)

add_executable(Aether WIN32 ${AETHER_SOURCES} bar/weather.cpp ${IMGUI_SOURCES})
target_include_directories(Aether PRIVATE include third_party/stb third_party/json ${IMGUI_DIR})
target_link_libraries(Aether PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore winmm ole32 oleaut32 windowsapp runtimeobject pdh winhttp shlwapi)
if(MSVC)
    target_compile_options(Aether PRIVATE /utf-8 /permissive- /EHsc)
endif()
# bundled theme wallpapers live next to the executables
add_custom_command(TARGET Aether POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/wallpapers $<TARGET_FILE_DIR:Aether>/wallpapers)

# ---- Aether Bar: standalone taskbar replacement (separate exe, same rendering stack) ----------
add_executable(AetherBar WIN32 bar/main.cpp bar/aether_bar.cpp bar/start_menu.cpp bar/tray_host.cpp bar/flyouts.cpp bar/weather.cpp bar/notifications.cpp bar/wifi.cpp bar/panels.cpp src/render/dcomp_layer.cpp src/render/context_menu.cpp)
target_include_directories(AetherBar PRIVATE include third_party/json)
target_link_libraries(AetherBar PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore shell32 ole32 oleaut32 uuid powrprof advapi32 iphlpapi wlanapi ws2_32 winhttp secur32 windowscodecs shlwapi dxguid)
if(MSVC)
    target_compile_options(AetherBar PRIVATE /utf-8 /permissive- /EHsc)
    # elevated, so the bar can talk to elevated apps (tray icons, drag & drop, window control)
    target_link_options(AetherBar PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()
