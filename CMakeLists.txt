cmake_minimum_required(VERSION 3.16)
project(EEG_CORE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------
# Paths (Windows LX SDK; edit only if needed)
# ---------------------------------------------------------
set(LXSDK_LIB     "${PROJECT_SOURCE_DIR}/device/lib/LXDeviceAPI.lib")

set(LXSDK_DLL_DIR "${PROJECT_SOURCE_DIR}/device/bin")
set(LXSDK_DLL     "${LXSDK_DLL_DIR}/LXDeviceAPI.dll")
set(LXSDK_CHART   "${LXSDK_DLL_DIR}/chartdir60.dll")
set(LXSDK_UTILDIR "${LXSDK_DLL_DIR}/LXDeviceAPI_Utility")

# =========================================================
# device library
#   - Windows: LXDeviceAPI (real device)
#   - macOS/Linux: stub/mock (buildable)
# =========================================================
add_library(device)

target_include_directories(device
    PUBLIC
        ${PROJECT_SOURCE_DIR}/device/include
        ${PROJECT_SOURCE_DIR}
)

target_sources(device PRIVATE
    device/source/unity_LSL.cpp
)

if(WIN32)
    target_sources(device PRIVATE
        device/source/eeg_LXDAPI.cpp
    )

    target_link_libraries(device PUBLIC
        ${LXSDK_LIB}
    )
else()
    target_sources(device PRIVATE
        device/source/eeg_LXDAPI_stub.cpp
    )

    target_compile_definitions(device PUBLIC
        EEG_NO_LXDEVICEAPI=1
    )
endif()

# =========================================================
# inference library
# =========================================================
add_library(inference
    inference/source/inference.cpp
)

target_include_directories(inference
    PUBLIC
        ${PROJECT_SOURCE_DIR}/inference/include
        ${PROJECT_SOURCE_DIR}
)

# =========================================================
# core library
# =========================================================
add_library(core
    core/source/controller.cpp
    core/source/ring_buffer.cpp
    core/source/epoch_buffer.cpp
)

target_include_directories(core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/core/include
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(core
    PUBLIC
        device
        inference
)

# =========================================================
# GUI deps: Dear ImGui + GLFW (vendored in external/)
#   external/imgui
#   external/glfw
# =========================================================
set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/external/imgui")
set(GLFW_DIR  "${PROJECT_SOURCE_DIR}/external/glfw")

# Build GLFW from vendored source
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

add_subdirectory(${GLFW_DIR} ${CMAKE_BINARY_DIR}/_deps/glfw)

# ImGui static library
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp

    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC glfw)

# =========================================================
# GUI application
#   apps/gui/main.cpp
#   apps/gui/eeg_gui.cpp
#   apps/gui/eeg_gui.hpp
# =========================================================
add_executable(eeg_gui
    apps/gui/main.cpp
    apps/gui/eeg_gui.cpp
)

target_include_directories(eeg_gui
    PRIVATE
        ${PROJECT_SOURCE_DIR}/apps/gui
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(eeg_gui
    PRIVATE
        core
        imgui
)

# OpenGL link (platform-specific)
if(APPLE)
    target_link_libraries(eeg_gui PRIVATE "-framework OpenGL")
elseif(WIN32)
    target_link_libraries(eeg_gui PRIVATE opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(eeg_gui PRIVATE OpenGL::GL)
endif()

# =========================================================
# Post-build: copy runtime DLLs next to exe (Windows only)
# =========================================================
if(WIN32)
    add_custom_command(TARGET eeg_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LXSDK_DLL}"
            "$<TARGET_FILE_DIR:eeg_gui>"

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LXSDK_CHART}"
            "$<TARGET_FILE_DIR:eeg_gui>"

        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${LXSDK_UTILDIR}"
            "$<TARGET_FILE_DIR:eeg_gui>/LXDeviceAPI_Utility"
    )
endif()
# =========================================================