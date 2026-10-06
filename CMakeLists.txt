cmake_minimum_required(VERSION 3.20)

project(C_AI_Movie_Shorts LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW) # timestamps of extracted archives
endif()

include(FetchContent)

# Some dependencies declare very old cmake_minimum_required() versions, which
# CMake 4.x refuses by default. Allow them to configure.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# ---------------- options: which apps to build ----------------
option(BUILD_CLI_APP       "Build CLI app (src/cli.c)"                         ON)
option(BUILD_RAYLIB_UI     "Build Raylib UI (src/main.c)"                      ON)
option(BUILD_WEB_UI        "Build the browser control panel (src/web_ui.c)"    ON)
option(BUILD_SIMPLE_UI     "Build the old simple Raylib UI (src/ui_main.c)"    OFF)
option(BUILD_WINDOWS_GUI   "Windows: build the UI without a console window"    OFF)
option(BUILD_UNIT_TESTS    "Build the unit tests (tests/)"                  OFF)

if(WIN32)
  set(_default_system_curl OFF)
else()
  set(_default_system_curl ON)
endif()
option(USE_SYSTEM_CURL "Use an installed libcurl (vcpkg/Homebrew/apt) instead of building it" ${_default_system_curl})

# Everything is linked statically -> a single self-contained .exe
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ---------------- raylib (UI) ----------------
if(BUILD_RAYLIB_UI OR BUILD_SIMPLE_UI)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
  set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    URL_HASH SHA256=aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
  )
  FetchContent_MakeAvailable(raylib)
endif()

# ---------------- cJSON (compiled directly into the core) ----------------
FetchContent_Declare(
  cjson
  URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
  URL_HASH SHA256=3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5
  SOURCE_SUBDIR _do_not_add_subdirectory_
)
FetchContent_MakeAvailable(cjson)

# ---------------- miniz (replaces the `unzip` tool for subtitle zips) ----------------
set(BUILD_FUZZERS   OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
set(BUILD_HEADER_ONLY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  miniz
  URL https://github.com/richgel999/miniz/archive/refs/tags/3.0.2.tar.gz
  URL_HASH SHA256=c4b4c25a4eb81883448ff8924e6dba95c800094a198dc9ce66a292ac2ef8e018
)
FetchContent_MakeAvailable(miniz)

# ---------------- libcurl ----------------
if(USE_SYSTEM_CURL)
  find_package(CURL REQUIRED)
else()
  # Build libcurl from source, HTTP(S) only, using Windows' native TLS (Schannel)
  # so no OpenSSL / certificate bundle is needed on Windows.
  set(BUILD_CURL_EXE        OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS     ON  CACHE BOOL "" FORCE)
  set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
  set(BUILD_LIBCURL_DOCS    OFF CACHE BOOL "" FORCE)
  set(BUILD_MISC_DOCS       OFF CACHE BOOL "" FORCE)
  set(ENABLE_CURL_MANUAL    OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_INSTALL  ON  CACHE BOOL "" FORCE)
  set(HTTP_ONLY             ON  CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL       OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2      OFF CACHE BOOL "" FORCE)
  set(USE_LIBIDN2           OFF CACHE BOOL "" FORCE)
  set(USE_NGHTTP2           OFF CACHE BOOL "" FORCE)
  set(CURL_BROTLI           OFF CACHE BOOL "" FORCE)
  set(CURL_ZSTD             OFF CACHE BOOL "" FORCE)
  set(CURL_ZLIB             OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAP     ON  CACHE BOOL "" FORCE)
  if(WIN32)
    set(CURL_USE_SCHANNEL   ON  CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL    OFF CACHE BOOL "" FORCE)
    set(USE_WIN32_IDN       ON  CACHE BOOL "" FORCE)
  else()
    set(CURL_USE_OPENSSL    ON  CACHE BOOL "" FORCE)
  endif()

  FetchContent_Declare(
    curl
    URL https://github.com/curl/curl/archive/refs/tags/curl-8_11_1.tar.gz
    URL_HASH SHA256=78baef817c33a0a11b990d031a706dc1486f614b581e22949fc101add32c4cc8
  )
  FetchContent_MakeAvailable(curl)
  if(NOT TARGET CURL::libcurl)
    add_library(CURL::libcurl ALIAS libcurl_static)
  endif()
endif()

find_package(Threads REQUIRED)

# ---------------- shared core library (NO main() here) ----------------
add_library(movie_core STATIC
  src/generator.c
  src/platform.c
  src/platform_open.c
  "${cjson_SOURCE_DIR}/cJSON.c"
)

target_include_directories(movie_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${cjson_SOURCE_DIR}"
)

target_link_libraries(movie_core PUBLIC
  CURL::libcurl
  miniz
  Threads::Threads
)

if(WIN32)
  # Unicode Win32 APIs, modern Windows (10+), ShellExecuteW
  target_compile_definitions(movie_core PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    CJSON_HIDE_SYMBOLS
  )
  target_link_libraries(movie_core PUBLIC shell32)
endif()

if(MINGW)
  # C99 printf (%zu, %lld) with MinGW's msvcrt
  target_compile_definitions(movie_core PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()

if(NOT WIN32)
  target_link_libraries(movie_core PUBLIC m)
endif()

# Warnings
if(MSVC)
  target_compile_definitions(movie_core PUBLIC _CRT_SECURE_NO_WARNINGS)
  target_compile_options(movie_core PRIVATE /W3 /utf-8)
else()
  target_compile_options(movie_core PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-const-variable
  )
endif()

# ---------------- Windows resources (icon + version info) ----------------
set(APP_RC "")
if(WIN32)
  enable_language(RC)
  set(APP_RC "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc")
endif()

# ---------------- CLI executable ----------------
if(BUILD_CLI_APP)
  add_executable(movie_summary_cli src/cli.c ${APP_RC})
  target_link_libraries(movie_summary_cli PRIVATE movie_core)
endif()

# ---------------- Web control panel executable ----------------
# Plain HTTP server + embedded single page UI: no raylib, no extra dependency.
if(BUILD_WEB_UI)
  add_executable(movie_summary_web src/web_ui.c ${APP_RC})
  target_link_libraries(movie_summary_web PRIVATE movie_core)
  if(WIN32)
    target_link_libraries(movie_summary_web PRIVATE ws2_32)
  endif()
endif()

# helper: UI target common settings
function(movie_shorts_ui_target tgt)
  target_link_libraries(${tgt} PRIVATE movie_core raylib)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /utf-8)
  endif()
  if(WIN32 AND BUILD_WINDOWS_GUI)
    set_target_properties(${tgt} PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
      # keep using main() as entry point
      target_link_options(${tgt} PRIVATE "/ENTRY:mainCRTStartup")
    endif()
  endif()
  if(APPLE)
    target_link_libraries(${tgt} PRIVATE
      "-framework Cocoa" "-framework IOKit" "-framework OpenGL"
      "-framework CoreVideo" "-framework CoreAudio" "-framework AudioToolbox")
  endif()
endfunction()

# ---------------- Raylib UI executable ----------------
if(BUILD_RAYLIB_UI)
  add_executable(movie_summary_bot src/main.c ${APP_RC})
  movie_shorts_ui_target(movie_summary_bot)
endif()

# ---------------- old simple UI (optional) ----------------
if(BUILD_SIMPLE_UI)
  add_executable(movie_summary_simple_ui src/ui_main.c ${APP_RC})
  movie_shorts_ui_target(movie_summary_simple_ui)
endif()

# ---------------- unit tests ----------------
# These compile the real pipeline sources (generator.c is included by the test
# so nothing has to be stubbed).  Run them from a scratch directory: they write
# scripts/srt_files/*.srt next to the current working directory.
if(BUILD_UNIT_TESTS)
  enable_testing()

  foreach(_t caption_language_test srt_structure_test)
    add_executable(${_t} tests/${_t}.c src/platform.c src/platform_open.c)
    target_include_directories(${_t} PRIVATE "${cjson_SOURCE_DIR}")
    target_link_libraries(${_t} PRIVATE CURL::libcurl miniz Threads::Threads)
    if(WIN32)
      target_compile_definitions(${_t} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX CJSON_HIDE_SYMBOLS)
      target_link_libraries(${_t} PRIVATE shell32)
    endif()
    add_test(NAME ${_t} COMMAND ${_t}
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_t}_run")
    set_tests_properties(${_t} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_t}_run")
  endforeach()

  # every test gets a scratch directory to write its subtitle fixtures in
  file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/caption_language_test_run"
    "${CMAKE_CURRENT_BINARY_DIR}/srt_structure_test_run")
endif()
