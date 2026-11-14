cmake_minimum_required(VERSION 3.20)
project(ExplorerTabUtility.Hook C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.3
)
# v1.3.3 does not ship a CMakeLists.txt, so build the (few) sources directly.
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
    "${minhook_SOURCE_DIR}/src/hde/hde64.c"
)
target_include_directories(minhook PUBLIC "${minhook_SOURCE_DIR}/include")
target_include_directories(minhook PRIVATE
    "${minhook_SOURCE_DIR}/src"
    "${minhook_SOURCE_DIR}/src/hde"
)

add_library(ExplorerTabUtility.Hook SHARED dllmain.cpp)
target_link_libraries(ExplorerTabUtility.Hook PRIVATE minhook user32 dwmapi)

set_target_properties(ExplorerTabUtility.Hook PROPERTIES
    OUTPUT_NAME "ExplorerTabUtility.Hook"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)
