cmake_minimum_required(VERSION 3.16)
project(CornerstoneDesigner C CXX)

# 构建时间戳：每次执行 build.bat 重新 configure 时刷新（关于/版本弹窗使用）
string(TIMESTAMP CORNERSTONE_BUILD_TIMESTAMP "%b %d %Y %H:%M:%S")

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
target_compile_definitions(CornerstoneDesigner PRIVATE
    CORNERSTONE_BUILD_TIMESTAMP="${CORNERSTONE_BUILD_TIMESTAMP}")

# Launcher：独立进程启动器 + 代码创建加载界面（不加载 JSON）
add_executable(CornerstoneLauncher WIN32
    src/Launcher.cpp
)

target_include_directories(CornerstoneLauncher PRIVATE
    include
    third_party/json/single_include
    ${UICORNERSTONE_ROOT}/binding/src)

target_link_libraries(CornerstoneLauncher PRIVATE UICornerstoneBinding)

if(WIN32)
    # GUI 子系统（无控制台窗口）；入口仍使用 main()，由 CRT 的 mainCRTStartup 接管。
    target_link_options(CornerstoneLauncher PRIVATE /ENTRY:mainCRTStartup)
endif()

if(WIN32)
    add_custom_command(TARGET CornerstoneLauncher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${UICORNERSTONE_ROOT}/UICornerstone.dll
            ${UICORNERSTONE_ROOT}/UIBackend_sdl3.dll
            ${UICORNERSTONE_ROOT}/SDL3.dll
            ${UICORNERSTONE_ROOT}/SDL3_ttf.dll
            ${UICORNERSTONE_ROOT}/SDL3_image.dll
            $<TARGET_FILE_DIR:CornerstoneLauncher>
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${UICORNERSTONE_ROOT}/assets
            $<TARGET_FILE_DIR:CornerstoneLauncher>/assets
    )
endif()

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
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/layouts/designer_theme.json
            $<TARGET_FILE_DIR:CornerstoneDesigner>/designer_theme.json
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/themes
            $<TARGET_FILE_DIR:CornerstoneDesigner>/themes
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
