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
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

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

add_executable(muq-viz
    src/main.cpp
    src/App.cpp
    src/AudioPipeWire.cpp)
target_compile_definitions(muq-viz PRIVATE GL_GLEXT_PROTOTYPES MUQ_VERSION="${PROJECT_VERSION}")
target_link_libraries(muq-viz PRIVATE
    muq_layout imgui
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
endif()
