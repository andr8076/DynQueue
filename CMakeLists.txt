cmake_minimum_required(VERSION 3.22)

project(DynQueue VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets)
find_package(KF6CoreAddons 6.0 REQUIRED)
find_package(KF6I18n 6.0 REQUIRED)
find_package(KF6XmlGui 6.0 REQUIRED)

set(DYNQUEUE_KONSOLE_VERSION "" CACHE STRING
    "Exact Konsole version used for the plugin build, for example 26.08.1")
if(NOT DYNQUEUE_KONSOLE_VERSION)
    find_program(DYNQUEUE_KONSOLE_EXECUTABLE konsole)
    if(DYNQUEUE_KONSOLE_EXECUTABLE)
        execute_process(
            COMMAND "${DYNQUEUE_KONSOLE_EXECUTABLE}" --version
            OUTPUT_VARIABLE DYNQUEUE_KONSOLE_VERSION_OUTPUT
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+"
               DYNQUEUE_KONSOLE_VERSION
               "${DYNQUEUE_KONSOLE_VERSION_OUTPUT}")
    endif()
endif()

if(NOT DYNQUEUE_KONSOLE_VERSION)
    find_program(DYNQUEUE_PACMAN_EXECUTABLE pacman)
    if(DYNQUEUE_PACMAN_EXECUTABLE)
        execute_process(
            COMMAND "${DYNQUEUE_PACMAN_EXECUTABLE}" -Q konsole
            OUTPUT_VARIABLE DYNQUEUE_KONSOLE_PACKAGE_OUTPUT
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+"
               DYNQUEUE_KONSOLE_VERSION
               "${DYNQUEUE_KONSOLE_PACKAGE_OUTPUT}")
    endif()
endif()

if(NOT DYNQUEUE_KONSOLE_VERSION)
    message(FATAL_ERROR
        "Could not determine the installed Konsole version. "
        "Pass -DDYNQUEUE_KONSOLE_VERSION=MAJOR.MINOR.MICRO.")
endif()

# Konsole does not currently install an external-plugin development package.
# The two libraries are nevertheless installed with Konsole and are loaded by
# the host process before this plugin.  The small compatibility header in
# src/plugin documents the ABI surface used here.
find_library(KONSOLE_APP_LIBRARY NAMES konsoleapp)
find_library(KONSOLE_PRIVATE_LIBRARY NAMES konsoleprivate)

if(NOT KONSOLE_APP_LIBRARY OR NOT KONSOLE_PRIVATE_LIBRARY)
    file(GLOB DYNQUEUE_KONSOLE_APP_CANDIDATES
        "/usr/lib/libkonsoleapp.so*"
        "/usr/lib64/libkonsoleapp.so*"
        "${CMAKE_PREFIX_PATH}/lib/libkonsoleapp.so*")
    file(GLOB DYNQUEUE_KONSOLE_PRIVATE_CANDIDATES
        "/usr/lib/libkonsoleprivate.so*"
        "/usr/lib64/libkonsoleprivate.so*"
        "${CMAKE_PREFIX_PATH}/lib/libkonsoleprivate.so*")

    if(NOT KONSOLE_APP_LIBRARY AND DYNQUEUE_KONSOLE_APP_CANDIDATES)
        list(SORT DYNQUEUE_KONSOLE_APP_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET DYNQUEUE_KONSOLE_APP_CANDIDATES 0 KONSOLE_APP_LIBRARY)
    endif()
    if(NOT KONSOLE_PRIVATE_LIBRARY AND DYNQUEUE_KONSOLE_PRIVATE_CANDIDATES)
        list(SORT DYNQUEUE_KONSOLE_PRIVATE_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET DYNQUEUE_KONSOLE_PRIVATE_CANDIDATES 0 KONSOLE_PRIVATE_LIBRARY)
    endif()
endif()

if(NOT KONSOLE_APP_LIBRARY OR NOT KONSOLE_PRIVATE_LIBRARY)
    message(FATAL_ERROR
        "Could not find libkonsoleapp and libkonsoleprivate. "
        "Install Konsole's runtime package or pass the library paths "
        "with -DKONSOLE_APP_LIBRARY=... and -DKONSOLE_PRIVATE_LIBRARY=....")
endif()

set(DYNQUEUE_PLUGIN_INSTALL_DIR "lib/qt6/plugins/konsoleplugins" CACHE PATH
    "Directory below the install prefix where the Konsole plugin is installed")

add_subdirectory(src)

install(FILES src/shell/dynqueue.bash DESTINATION share/dynqueue)
install(PROGRAMS src/shell/dynqueue-parser.py DESTINATION libexec)
install(PROGRAMS tools/dynqueue-refresh.sh DESTINATION libexec RENAME dynqueue-refresh)
