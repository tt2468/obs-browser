project(obs-browser)

option(
  ENABLE_BROWSER
  "Enable building OBS with browser source plugin (required Chromium Embedded Framework)"
  ${OS_LINUX})

if(NOT ENABLE_BROWSER)
  message(STATUS "OBS:  DISABLED   obs-browser")
  message(
    WARNING
      "Browser source support is not enabled by default - please switch ENABLE_BROWSER to ON and specify CEF_ROOT_DIR to enable this functionality."
  )
  return()
endif()

find_package(CEF REQUIRED)
find_package(nlohmann_json REQUIRED)

if(NOT TARGET CEF::Wrapper)
  message(
    FATAL_ERROR
      "OBS:    -        Unable to find CEF Libraries - set CEF_ROOT_DIR or configure with ENABLE_BROWSER=OFF"
  )
endif()

add_library(obs-browser MODULE)
add_library(OBS::browser ALIAS obs-browser)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/browser-config.h.in
               ${CMAKE_BINARY_DIR}/config/browser-config.h)

target_sources(
  obs-browser
  PRIVATE obs-browser-plugin.cpp
          obs-browser-source.cpp
          obs-browser-source.hpp
          browser-app.cpp
          browser-app.hpp
          browser-client.cpp
          browser-client.hpp
          browser-scheme.cpp
          browser-scheme.hpp
          browser-version.h
          cef-headers.hpp
          deps/base64/base64.cpp
          deps/base64/base64.hpp
          deps/wide-string.cpp
          deps/wide-string.hpp
          deps/signal-restore.cpp
          deps/signal-restore.hpp
          deps/obs-websocket-api/obs-websocket-api.h
          ${CMAKE_BINARY_DIR}/config/browser-config.h)

target_include_directories(obs-browser PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps
                                               ${CMAKE_BINARY_DIR}/config)

target_link_libraries(obs-browser PRIVATE OBS::libobs OBS::frontend-api nlohmann_json::nlohmann_json)

target_compile_features(obs-browser PRIVATE cxx_std_17)

# BROWSER PAGE
add_executable(obs-browser-page)

target_sources(
  obs-browser-page
  PRIVATE cef-headers.hpp obs-browser-page/obs-browser-page-main.cpp
          browser-app.cpp browser-app.hpp)

target_link_libraries(obs-browser-page PRIVATE CEF::Library)

target_include_directories(
  obs-browser-page
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/deps
          ${CMAKE_CURRENT_SOURCE_DIR}/obs-browser-page)

target_compile_features(obs-browser-page PRIVATE cxx_std_17)

target_link_libraries(obs-browser-page PRIVATE CEF::Wrapper)

set_target_properties(obs-browser-page PROPERTIES FOLDER "plugins/obs-browser")

setup_plugin_target(obs-browser-page)
#####

if(OS_POSIX)
  find_package(X11 REQUIRED)

  target_link_libraries(obs-browser PRIVATE CEF::Wrapper CEF::Library X11::X11)

  get_target_property(_CEF_DIRECTORY CEF::Library INTERFACE_LINK_DIRECTORIES)

  set_target_properties(obs-browser PROPERTIES BUILD_RPATH "$ORIGIN/")

  set_target_properties(obs-browser-page PROPERTIES BUILD_RPATH "$ORIGIN/")

  set_target_properties(obs-browser PROPERTIES INSTALL_RPATH "$ORIGIN/")
  set_target_properties(obs-browser-page PROPERTIES INSTALL_RPATH "$ORIGIN/")
endif()

set_target_properties(obs-browser PROPERTIES FOLDER "plugins/obs-browser" PREFIX "")

setup_plugin_target(obs-browser)
