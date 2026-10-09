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
# 2. Tìm MySQL C API (Tương thích cả Windows & macOS)
# =====================================================
set(MYSQL_INCLUDE_SEARCH_PATHS "")
set(MYSQL_LIB_SEARCH_PATHS "")

if(WIN32)
    # Đường dẫn mặc định cài đặt MySQL trên Windows
    list(APPEND MYSQL_INCLUDE_SEARCH_PATHS 
        "C:/Program Files/MySQL/MySQL Server 8.0/include"
        "C:/Program Files/MySQL/MySQL Server 8.4/include"
        "C:/Program Files/MySQL/MySQL Connector C 6.1/include"
        "C:/Program Files (x86)/MySQL/MySQL Connector C 6.1/include"
    )
    list(APPEND MYSQL_LIB_SEARCH_PATHS 
        "C:/Program Files/MySQL/MySQL Server 8.0/lib"
        "C:/Program Files/MySQL/MySQL Server 8.4/lib"
        "C:/Program Files/MySQL/MySQL Connector C 6.1/lib"
        "C:/Program Files (x86)/MySQL/MySQL Connector C 6.1/lib"
    )
elseif(APPLE)
    # Đường dẫn Homebrew trên macOS
    find_program(BREW_EXECUTABLE NAMES brew PATHS /opt/homebrew/bin /usr/local/bin)
    if(BREW_EXECUTABLE)
        execute_process(
            COMMAND ${BREW_EXECUTABLE} --prefix mysql-client
            OUTPUT_VARIABLE MYSQL_CLIENT_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(MYSQL_CLIENT_PREFIX AND EXISTS "${MYSQL_CLIENT_PREFIX}")
            list(APPEND MYSQL_INCLUDE_SEARCH_PATHS "${MYSQL_CLIENT_PREFIX}/include" "${MYSQL_CLIENT_PREFIX}/include/mysql")
            list(APPEND MYSQL_LIB_SEARCH_PATHS "${MYSQL_CLIENT_PREFIX}/lib")
        endif()
    endif()
endif()

find_path(
    MYSQL_INCLUDE_DIR
    NAMES mysql.h
    PATHS ${MYSQL_INCLUDE_SEARCH_PATHS}
)

find_library(
    MYSQL_LIBRARY
    NAMES mysqlclient libmysql
    PATHS ${MYSQL_LIB_SEARCH_PATHS}
)

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    if(WIN32)
        message(FATAL_ERROR "Không tìm thấy MySQL C API trên Windows. Hãy kiểm tra đường dẫn cài đặt C:/Program Files/MySQL/")
    else()
        message(FATAL_ERROR "Không tìm thấy mysql-client. Hãy chạy: brew install mysql-client")
    endif()
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

# Tự động copy file libmysql.dll vào thư mục build trên Windows
if(WIN32)
    get_filename_component(MYSQL_LIB_DIR ${MYSQL_LIBRARY} DIRECTORY)
    find_file(MYSQL_DLL NAMES libmysql.dll PATHS ${MYSQL_LIB_DIR} "${MYSQL_LIB_DIR}/../bin")
    if(MYSQL_DLL)
        add_custom_command(TARGET energy_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MYSQL_DLL}"
            $<TARGET_FILE_DIR:energy_app>
        )
    endif()
endif()