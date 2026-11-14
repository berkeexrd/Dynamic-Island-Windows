cmake_minimum_required(VERSION 3.20)
project(Aether LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()

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

add_executable(Aether WIN32 ${AETHER_SOURCES} bar/weather.cpp ${IMGUI_SOURCES} assets/icons/aether.rc)
target_include_directories(Aether PRIVATE include third_party/stb third_party/json ${IMGUI_DIR})
target_link_libraries(Aether PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore winmm ole32 oleaut32 windowsapp runtimeobject pdh winhttp shlwapi)
if(MSVC)
    target_compile_options(Aether PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/permissive-;/EHsc>")
endif()
# bundled theme wallpapers live next to the executables
add_custom_command(TARGET Aether POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/wallpapers $<TARGET_FILE_DIR:Aether>/wallpapers)

# ---- Aether Bar: standalone taskbar replacement (separate exe, same rendering stack) ----------
add_executable(AetherBar WIN32 bar/main.cpp bar/aether_bar.cpp bar/start_menu.cpp bar/tray_host.cpp bar/flyouts.cpp bar/weather.cpp bar/notifications.cpp bar/wifi.cpp bar/panels.cpp src/render/dcomp_layer.cpp src/render/context_menu.cpp assets/icons/aether.rc)
target_include_directories(AetherBar PRIVATE include third_party/json)
target_link_libraries(AetherBar PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore shell32 ole32 oleaut32 uuid powrprof advapi32 iphlpapi wlanapi ws2_32 winhttp secur32 windowscodecs shlwapi dxguid)
if(MSVC)
    target_compile_options(AetherBar PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/permissive-;/EHsc>")
    # elevated, so the bar can talk to elevated apps (tray icons, drag & drop, window control)
    target_link_options(AetherBar PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

# ---- Aether Explorer: from-scratch file manager (separate exe, same rendering stack) ----------
add_executable(AetherExplorer WIN32 explorer/main.cpp src/render/dcomp_layer.cpp src/render/context_menu.cpp assets/icons/aether_explorer.rc)
target_include_directories(AetherExplorer PRIVATE include explorer third_party/json)
target_link_libraries(AetherExplorer PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore shell32 ole32 oleaut32 uuid advapi32 secur32 windowscodecs shlwapi dxguid wininet mpr crypt32)
if(MSVC)
    target_compile_options(AetherExplorer PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/permissive-;/EHsc>")
endif()

# ---- Aether Desktop: wallpaper + desktop icons in the Aether style (separate exe) --------------
add_executable(AetherDesktop WIN32 desktop/main.cpp src/render/dcomp_layer.cpp src/render/context_menu.cpp assets/icons/aether.rc)
target_include_directories(AetherDesktop PRIVATE include explorer third_party/json)
target_link_libraries(AetherDesktop PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore shell32 ole32 oleaut32 uuid advapi32 secur32 windowscodecs shlwapi dxguid)
if(MSVC)
    target_compile_options(AetherDesktop PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/permissive-;/EHsc>")
endif()

# ---- Aether Shell: session manager (starts/supervises bar, desktop, island; Alt+Tab, Win+R ...) ----------
add_executable(AetherShell WIN32 shell/main.cpp src/render/dcomp_layer.cpp assets/icons/aether.rc)
target_include_directories(AetherShell PRIVATE include third_party/json)
target_link_libraries(AetherShell PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore shell32 ole32 oleaut32 uuid advapi32 windowscodecs shlwapi dxguid psapi)
if(MSVC)
    target_compile_options(AetherShell PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/permissive-;/EHsc>")
endif()

# ---- Aether Bin: Recycle Bin window ------------------------------------------------------------------------------
add_executable(AetherBin WIN32 bin/main.cpp src/render/dcomp_layer.cpp assets/icons/aether_explorer.rc)
target_include_directories(AetherBin PRIVATE include third_party/json)
target_link_libraries(AetherBin PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore shell32 ole32 oleaut32 uuid advapi32 windowscodecs shlwapi dxguid comctl32)
if(MSVC)
    target_compile_options(AetherBin PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/permissive-;/EHsc>")
endif()

# ---- Aether Intro: greeting animation shown at logon / first run ----------------------------------------------------
add_executable(AetherIntro WIN32 intro/main.cpp src/render/dcomp_layer.cpp intro/intro.rc)
target_include_directories(AetherIntro PRIVATE include)
target_link_libraries(AetherIntro PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore shell32 ole32 oleaut32 uuid windowscodecs shlwapi dxguid)
if(MSVC)
    target_compile_options(AetherIntro PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/permissive-;/EHsc>")
endif()

# ---- Aether Setup: single-file installer (all programs are embedded as installer/payload zip) -------------------------
set(AETHER_STAGE ${CMAKE_BINARY_DIR}/payload-stage)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/payload.zip
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${AETHER_STAGE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${AETHER_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Aether> $<TARGET_FILE:AetherBar> $<TARGET_FILE:AetherDesktop> $<TARGET_FILE:AetherExplorer> $<TARGET_FILE:AetherShell> $<TARGET_FILE:AetherBin> $<TARGET_FILE:AetherIntro> ${AETHER_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/wallpapers ${AETHER_STAGE}/wallpapers
    COMMAND ${CMAKE_COMMAND} -E chdir ${AETHER_STAGE} ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/payload.zip --format=zip .
    DEPENDS Aether AetherBar AetherDesktop AetherExplorer AetherShell AetherBin AetherIntro
    VERBATIM)
add_executable(AetherSetup WIN32 installer/main.cpp src/render/dcomp_layer.cpp installer/setup.rc ${CMAKE_BINARY_DIR}/payload.zip)
set_source_files_properties(${CMAKE_BINARY_DIR}/payload.zip PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
set_source_files_properties(installer/setup.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/payload.zip;${CMAKE_SOURCE_DIR}/assets/icons/aether.png")
target_include_directories(AetherSetup PRIVATE include ${CMAKE_BINARY_DIR})
target_link_libraries(AetherSetup PRIVATE d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore shell32 ole32 oleaut32 uuid advapi32 windowscodecs shlwapi dxguid)
if(MSVC)
    target_compile_options(AetherSetup PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/permissive-;/EHsc>")
    target_link_options(AetherSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()
