cmake_minimum_required(VERSION 3.20)

project(trail VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# kcoreaddons_add_plugin() builds a static plugin when BUILD_SHARED_LIBS is off,
# but KWin can only load this effect as a dynamically loaded plugin.
set(BUILD_SHARED_LIBS ON)
# Make AUTOMOC recognise the effect factory macros, so that it generates the
# plugin metadata (otherwise it warns and relies on the #include "main.moc").
set(CMAKE_AUTOMOC_MACRO_NAMES
    ${CMAKE_AUTOMOC_MACRO_NAMES}
    KWIN_EFFECT_FACTORY
    KWIN_EFFECT_FACTORY_SUPPORTED
    KWIN_EFFECT_FACTORY_ENABLED
    KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED
)
# kcoreaddons_add_plugin() requires this to be set.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
# KDEInstallDirs supersedes GNUInstallDirs and must be included first.
include(KDEInstallDirs)

# KWin looks for effect plugins in Qt's plugin directory, i.e. the qt6 subdirectory
# of the library dir, not in <prefix>/<libdir>/plugins. ECM only appends the qt6
# component when the install prefix equals Qt's own prefix, so state it explicitly.
set(KDE_INSTALL_PLUGINDIR "${KDE_INSTALL_LIBDIR}/qt6/plugins")

# Widgets and DBus are not used directly, but the imported KWin::kwin target
# lists them in its link interface, so the imported targets have to exist.
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets DBus)
find_package(KF6CoreAddons REQUIRED)
find_package(KWin REQUIRED)

# Some installed CMake packages hard-code absolute paths from the machine they
# were built on (e.g. /usr/include/KF6/KConfig or
# /usr/lib64/qt6/metatypes/qt6kf6configcore_metatypes.json). On a normal system
# those exist; when building against a relocated prefix (CMAKE_PREFIX_PATH
# pointing somewhere other than /usr) they do not, and CMake refuses to
# generate. Translate such a path to the equivalent path inside the prefix when
# the prefix provides it, and drop it when it is genuinely absent (in that case
# it belongs to an optional feature this effect does not use).
function(trail_relocate_imported_paths)
    get_property(_targets DIRECTORY PROPERTY IMPORTED_TARGETS)
    foreach(_target IN LISTS _targets)
        foreach(_property INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SOURCES)
            get_target_property(_paths ${_target} ${_property})
            if(NOT _paths)
                continue()
            endif()
            set(_fixed "")
            foreach(_path IN LISTS _paths)
                if(EXISTS "${_path}")
                    list(APPEND _fixed "${_path}")
                    continue()
                endif()
                string(REGEX REPLACE "^/usr/?" "" _relative "${_path}")
                foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
                    if(EXISTS "${_prefix}/${_relative}")
                        list(APPEND _fixed "${_prefix}/${_relative}")
                        break()
                    endif()
                endforeach()
            endforeach()
            list(REMOVE_DUPLICATES _fixed)
            set_target_properties(${_target} PROPERTIES ${_property} "${_fixed}")
        endforeach()
    endforeach()
endfunction()

trail_relocate_imported_paths()

add_subdirectory(src)
