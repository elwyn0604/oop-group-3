cmake_minimum_required(VERSION 3.16)
project(energy_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =====================================================
# 1. Tìm nlohmann/json
# =====================================================
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    set(JSON_TARGET nlohmann_json::nlohmann_json)
else()
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
    set(JSON_TARGET nlohmann_json::nlohmann_json)
endif()

# =====================================================
# 2. Tìm MySQL C API (mysql-client trên macOS)
# =====================================================
set(MYSQL_SEARCH_PREFIXES "")

if(APPLE)
    find_program(BREW_EXECUTABLE NAMES brew PATHS /opt/homebrew/bin /usr/local/bin)
    if(BREW_EXECUTABLE)
        execute_process(
            COMMAND ${BREW_EXECUTABLE} --prefix
            OUTPUT_VARIABLE HOMEBREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        execute_process(
            COMMAND ${BREW_EXECUTABLE} --prefix mysql-client
            OUTPUT_VARIABLE MYSQL_CLIENT_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(HOMEBREW_PREFIX AND EXISTS "${HOMEBREW_PREFIX}")
            list(APPEND MYSQL_SEARCH_PREFIXES "${HOMEBREW_PREFIX}")
        endif()
        if(MYSQL_CLIENT_PREFIX AND EXISTS "${MYSQL_CLIENT_PREFIX}")
            list(APPEND MYSQL_SEARCH_PREFIXES "${MYSQL_CLIENT_PREFIX}")
        endif()
    endif()
endif()

set(MYSQL_INCLUDE_SEARCH_PATHS "")
set(MYSQL_LIB_SEARCH_PATHS "")

foreach(prefix IN LISTS MYSQL_SEARCH_PREFIXES)
    list(APPEND MYSQL_INCLUDE_SEARCH_PATHS "${prefix}/include")
    list(APPEND MYSQL_INCLUDE_SEARCH_PATHS "${prefix}/include/mysql")
    list(APPEND MYSQL_LIB_SEARCH_PATHS "${prefix}/lib")
endforeach()

find_path(
    MYSQL_INCLUDE_DIR
    NAMES mysql.h
    PATHS ${MYSQL_INCLUDE_SEARCH_PATHS}
)

find_library(
    MYSQL_LIBRARY
    NAMES mysqlclient
    PATHS ${MYSQL_LIB_SEARCH_PATHS}
)

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(FATAL_ERROR "Không tìm thấy mysql-client. Hãy chạy: brew install mysql-client")
endif()

# =====================================================
# 3. Tạo chương trình
# =====================================================
add_executable(energy_app main.cpp)

target_include_directories(
    energy_app
    PRIVATE
    ${MYSQL_INCLUDE_DIR}
)

target_link_libraries(
    energy_app
    PRIVATE
    ${JSON_TARGET}
    ${MYSQL_LIBRARY}
)