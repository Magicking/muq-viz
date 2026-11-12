cmake_minimum_required(VERSION 3.21)
project(muq-viz VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(MUQ_PORTABLE "Statically link libstdc++/libgcc for a portable binary" ON)
option(MUQ_BUILD_TESTS "Build unit tests" ON)

set(OpenGL_GL_PREFERENCE LEGACY)
find_package(OpenGL REQUIRED)
find_package(SDL2 REQUIRED CONFIG)
find_package(projectM4 REQUIRED COMPONENTS Playlist)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()
# Header-only; populate without its CMakeLists (avoids optional OpenSSL/zlib/brotli probing).
FetchContent_GetProperties(httplib)
if(NOT httplib_POPULATED)
    FetchContent_Populate(httplib)
endif()

# Embed the web control page into the binary (no runtime file dependency).
set(MUQ_WEB_INDEX ${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MUQ_WEB_INDEX})
file(READ ${MUQ_WEB_INDEX} MUQ_WEB_INDEX_HTML)
configure_file(src/web/WebAssets.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/WebAssets.cpp @ONLY)

if(TARGET SDL2::SDL2-static)
    set(MUQ_SDL SDL2::SDL2-static)
else()
    set(MUQ_SDL SDL2::SDL2)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC ${MUQ_SDL})

# Layout parser as separate lib (unit-testable without GL/SDL)
add_library(muq_layout STATIC src/ObsLayout.cpp)
target_include_directories(muq_layout PUBLIC src)
target_link_libraries(muq_layout PUBLIC nlohmann_json::nlohmann_json)

# INI config load/save (unit-testable without GL/SDL)
add_library(muq_config STATIC src/Config.cpp)
target_link_libraries(muq_config PUBLIC muq_layout)

add_executable(muq-viz
    src/main.cpp
    src/App.cpp
    src/AudioPipeWire.cpp
    src/WebServer.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/WebAssets.cpp)
target_include_directories(muq-viz PRIVATE ${httplib_SOURCE_DIR})
target_compile_definitions(muq-viz PRIVATE GL_GLEXT_PROTOTYPES MUQ_VERSION="${PROJECT_VERSION}")
target_link_libraries(muq-viz PRIVATE
    muq_layout muq_config imgui
    libprojectM::playlist libprojectM::projectM
    ${MUQ_SDL} PkgConfig::PIPEWIRE OpenGL::GL Threads::Threads ${CMAKE_DL_LIBS})
if(MUQ_PORTABLE)
    target_link_options(muq-viz PRIVATE -static-libstdc++ -static-libgcc)
endif()
install(TARGETS muq-viz RUNTIME DESTINATION bin)

if(MUQ_BUILD_TESTS)
    enable_testing()
    add_executable(test_layout tests/test_layout.cpp)
    target_link_libraries(test_layout PRIVATE muq_layout)
    add_test(NAME layout COMMAND test_layout ${CMAKE_CURRENT_SOURCE_DIR}/layouts/Aeriechaffaudage1024x768_4.json)
    add_executable(test_config tests/test_config.cpp)
    target_link_libraries(test_config PRIVATE muq_config)
    add_test(NAME config COMMAND test_config ${CMAKE_CURRENT_BINARY_DIR}/test-config-tmp)
endif()
