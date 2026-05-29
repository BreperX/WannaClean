cmake_minimum_required(VERSION 3.20)
project(WannaClean LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# DEPENDENCY VALIDATION
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")

if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR 
        "\n[ERROR] Could not find the 'imgui/' folder.\n"
        "Please clone ImGui by running the following command in the project root:\n"
        "git clone --depth=1 https://github.com/ocornut/imgui.git\n"
    )
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WannaClean.ico")
    message(WARNING "WannaClean.ico not found. The application will use the default icon.")
endif()

# COMPILER SETTINGS (MSVC)

if(MSVC)
    add_compile_options(/W3 /MP /EHsc)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    add_compile_options(/FI${CMAKE_CURRENT_SOURCE_DIR}/win_compat.h)
endif()

# PROJECT SOURCES
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

set(APP_SOURCES
    main.cpp
    config.cpp
    process.cpp
    resources.cpp
    profiles.cpp
    ui_main.cpp
    ui_result.cpp
    app.rc
)

add_executable(WannaClean WIN32 
    ${APP_SOURCES} 
    ${IMGUI_SOURCES}
)


# INCLUDES, LIBRARIES AND TARGET OPTIONS
target_include_directories(WannaClean PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

target_link_libraries(WannaClean PRIVATE
    kernel32 user32 gdi32 shell32 comctl32 
    d3d11 dxgi pdh ntdll
)

if(MSVC)
    target_link_options(WannaClean PRIVATE "/MANIFEST:NO" "/SUBSYSTEM:WINDOWS")
endif()