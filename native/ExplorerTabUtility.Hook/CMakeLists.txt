cmake_minimum_required(VERSION 3.20)
project(ExplorerTabUtility.Hook CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.3
)
FetchContent_MakeAvailable(minhook)

add_library(ExplorerTabUtility.Hook SHARED dllmain.cpp)
target_include_directories(ExplorerTabUtility.Hook PRIVATE "${minhook_SOURCE_DIR}/include")
target_link_libraries(ExplorerTabUtility.Hook PRIVATE minhook user32)

set_target_properties(ExplorerTabUtility.Hook PROPERTIES
    OUTPUT_NAME "ExplorerTabUtility.Hook"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)
