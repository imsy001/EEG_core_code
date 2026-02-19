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

# ---------------------------------------------------------
# liblsl paths (your layout)
# ---------------------------------------------------------
set(LSL_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/external/liblsl/include")
set(LSL_LIB         "${PROJECT_SOURCE_DIR}/external/liblsl/lib/lsl.lib")
set(LSL_DLL         "${PROJECT_SOURCE_DIR}/external/liblsl/bin/lsl.dll")

# ---------------------------------------------------------
# MSVC: source encoding (avoid codepage 949 warnings)
# ---------------------------------------------------------
if(MSVC)
    add_compile_options(/utf-8)
endif()

# =========================================================
# unity_lsl library  (ONLY place that compiles LSL sources)
# =========================================================
add_library(unity_lsl STATIC
    unity_LSL/source/unity_LSL.cpp
    unity_LSL/source/lsl_reader.cpp
    unity_LSL/source/lsl_bridge_impl.cpp
)

target_include_directories(unity_lsl
    PUBLIC
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/unity_LSL/include
        ${LSL_INCLUDE_DIR}
)

# Link liblsl binary (Windows)
if(WIN32)
    target_link_libraries(unity_lsl PUBLIC ${LSL_LIB})
endif()

# =========================================================
# device library
# =========================================================
add_library(device)

target_include_directories(device
    PUBLIC
        ${PROJECT_SOURCE_DIR}/device/include
        ${PROJECT_SOURCE_DIR}
)

if(WIN32)
    target_sources(device PRIVATE
        device/source/eeg_LXDAPI.cpp
    )

    target_link_libraries(device PUBLIC
        ${LXSDK_LIB}
        unity_lsl
    )
else()
    target_sources(device PRIVATE
        device/source/eeg_LXDAPI_stub.cpp
    )

    target_compile_definitions(device PUBLIC
        EEG_NO_LXDEVICEAPI=1
    )

    target_link_libraries(device PUBLIC
        unity_lsl
    )
endif()

# ---------------------------------------------------------
# ONNX Runtime (official prebuilt) paths
# ---------------------------------------------------------
set(ORT_DIR          "${PROJECT_SOURCE_DIR}/external/onnxruntime")
set(ORT_INCLUDE_DIR  "${ORT_DIR}/include")
set(ORT_LIB_DIR      "${ORT_DIR}/lib")

# DLL may be in lib or bin depending on the package
set(ORT_DLL_LIB      "${ORT_LIB_DIR}/onnxruntime.dll")
set(ORT_DLL_BIN      "${ORT_DIR}/bin/onnxruntime.dll")

if(EXISTS "${ORT_DLL_BIN}")
    set(ORT_DLL "${ORT_DLL_BIN}")
elseif(EXISTS "${ORT_DLL_LIB}")
    set(ORT_DLL "${ORT_DLL_LIB}")
else()
    message(FATAL_ERROR "onnxruntime.dll not found in ${ORT_DIR}/bin or ${ORT_DIR}/lib")
endif()

if(NOT EXISTS "${ORT_LIB_DIR}/onnxruntime.lib")
    message(FATAL_ERROR "onnxruntime.lib not found at ${ORT_LIB_DIR}/onnxruntime.lib")
endif()

# ---------------------------------------------------------
# Define onnxruntime imported target (Windows)
# ---------------------------------------------------------
if(WIN32)
    add_library(onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${ORT_LIB_DIR}/onnxruntime.lib"
        IMPORTED_LOCATION "${ORT_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}"
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

target_link_libraries(inference
    PUBLIC
        unity_lsl
        onnxruntime
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
        unity_lsl
)

# =========================================================
# GUI deps: Dear ImGui + GLFW (vendored in external/)
# =========================================================
set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/external/imgui")
set(GLFW_DIR  "${PROJECT_SOURCE_DIR}/external/glfw")

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

add_subdirectory(${GLFW_DIR} ${CMAKE_BINARY_DIR}/_deps/glfw)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui
    PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC glfw)

# =========================================================
# GUI application
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
        unity_lsl
)

# OpenGL link
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

        # ✅ liblsl runtime
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LSL_DLL}"
            "$<TARGET_FILE_DIR:eeg_gui>"

         # ✅ onnxruntime runtime
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORT_DLL}"
            "$<TARGET_FILE_DIR:eeg_gui>"
    )
endif()


