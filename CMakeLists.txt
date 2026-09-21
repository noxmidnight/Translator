cmake_minimum_required(VERSION 3.16)
project(translator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

add_executable(translator
  src/main.cpp
  src/config.cpp
  src/llama_client.cpp
  src/prompts.cpp
  src/lexicon.cpp
)

target_include_directories(translator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(translator PRIVATE PkgConfig::GTK3 PkgConfig::CURL)

install(TARGETS translator RUNTIME DESTINATION bin)
install(DIRECTORY data/ DESTINATION share/translator/data)
install(DIRECTORY assets/ DESTINATION share/translator/assets)
