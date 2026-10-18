cmake_minimum_required(VERSION 3.16)
project(CornerstoneDesigner C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_options(/utf-8 /FS)
endif()

set(UICORNERSTONE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/subModules/UICornerstone)

# C++ Binding 静态库（独立于核心库构建；经 LoadLibrary 动态加载核心/后端 DLL）
# 发布包无核心构建树：置空跳过 binding 自带样例的构建（参照 binding/CMakeLists.example.txt）
set(UICORNERSTONE_CORE_OUTPUT "" CACHE PATH "核心库产物目录（发布包置空）")
add_subdirectory(${UICORNERSTONE_ROOT}/binding ${CMAKE_BINARY_DIR}/binding)

# 设计器
add_executable(CornerstoneDesigner
    src/main.cpp
    src/App.cpp
    src/ControlModel.cpp
    src/CanvasPane.cpp
    app.manifest            # longPathAware（系统启用 LongPathsEnabled 时文件对话框等全链支持长路径；
                            # 未启用时 .csd/最近文件等 I/O 仍经 \\?\ 前缀规避 MAX_PATH）
)

target_include_directories(CornerstoneDesigner PRIVATE
    include
    third_party/json/single_include
    ${UICORNERSTONE_ROOT}/binding/src)   # DynamicApi.h（fnSetCallback 等 C ABI 动态解析）
target_link_libraries(CornerstoneDesigner PRIVATE UICornerstoneBinding)

# 运行依赖 POST_BUILD 拷贝（参照 UICS binding/CMakeLists.txt 模式）：
#   UICornerstone.dll + UIBackend_sdl3.dll + SDL3*.dll + assets/ + layouts/
#   来源：subModules/UICornerstone/ 与仓库 layouts/
if(WIN32)
    add_custom_command(TARGET CornerstoneDesigner POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${UICORNERSTONE_ROOT}/UICornerstone.dll
            ${UICORNERSTONE_ROOT}/UIBackend_sdl3.dll
            ${UICORNERSTONE_ROOT}/SDL3.dll
            ${UICORNERSTONE_ROOT}/SDL3_ttf.dll
            ${UICORNERSTONE_ROOT}/SDL3_image.dll
            $<TARGET_FILE_DIR:CornerstoneDesigner>
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${UICORNERSTONE_ROOT}/assets
            $<TARGET_FILE_DIR:CornerstoneDesigner>/assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/gen/icons
            $<TARGET_FILE_DIR:CornerstoneDesigner>/gen/icons
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/layouts/main_layout.json
            $<TARGET_FILE_DIR:CornerstoneDesigner>/main_layout.json
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:CornerstoneDesigner>/schema
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${UICORNERSTONE_ROOT}/docs/schema/declarative-ui.schema.json
            $<TARGET_FILE_DIR:CornerstoneDesigner>/schema/declarative-ui.schema.json
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:CornerstoneDesigner>/sdk/include
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${UICORNERSTONE_ROOT}/binding/include/UICornerstoneAPI.h
            ${UICORNERSTONE_ROOT}/binding/include/PropertyNames.h
            $<TARGET_FILE_DIR:CornerstoneDesigner>/sdk/include
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${UICORNERSTONE_ROOT}/binding
            $<TARGET_FILE_DIR:CornerstoneDesigner>/sdk/binding
    )
endif()
