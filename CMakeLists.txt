cmake_minimum_required(VERSION 3.16)
project(emacs-sync VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Files baked into the binary. Format: <id>=<path>. The id is what the C++ code
# uses to look the file up (see kConfigFiles in src/main.cpp for where each one is written).
set(EMBED_INPUTS
  "dot_emacs=${CMAKE_SOURCE_DIR}/config/dot-emacs.el"
  "emacs_custom=${CMAKE_SOURCE_DIR}/config/emacs-custom.el"
  "bootstrap=${CMAKE_SOURCE_DIR}/resources/bootstrap.el"
)
set(EMBED_DEPENDS)
foreach(entry IN LISTS EMBED_INPUTS)
  string(REGEX REPLACE "^[^=]+=" "" path "${entry}")
  list(APPEND EMBED_DEPENDS "${path}")
endforeach()

set(EMBED_OUT "${CMAKE_BINARY_DIR}/generated/embedded_files.cpp")
string(REPLACE ";" "|" EMBED_INPUTS_ARG "${EMBED_INPUTS}")
add_custom_command(
  OUTPUT "${EMBED_OUT}"
  COMMAND ${CMAKE_COMMAND} "-DOUT=${EMBED_OUT}" "-DINPUTS=${EMBED_INPUTS_ARG}"
          -P "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS ${EMBED_DEPENDS} "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  COMMENT "Embedding Emacs config files"
  VERBATIM
)

add_executable(emacs-sync
  src/main.cpp
  src/platform.cpp
  "${EMBED_OUT}"
)
target_include_directories(emacs-sync PRIVATE src)
target_compile_definitions(emacs-sync PRIVATE
  EMACS_SYNC_VERSION="${PROJECT_VERSION}"
  EMACS_SYNC_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

if(MSVC)
  target_compile_options(emacs-sync PRIVATE /W4 /utf-8)
else()
  target_compile_options(emacs-sync PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Make the Linux binary portable across distros: no dependency on the host's
# libstdc++ version.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_link_options(emacs-sync PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS emacs-sync RUNTIME DESTINATION bin)
