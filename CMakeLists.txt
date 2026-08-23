# ==============================================================================
# Lumen Photo Studio V1 (post-correction-pass)
# Non-destructive photo editing engine — Qt 6, C++20, linear-light workflow
# ==============================================================================
cmake_minimum_required(VERSION 3.21)

project(LumenPhotoStudio
    VERSION 1.1.0
    DESCRIPTION "Non-destructive photo editing engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(GNUInstallDirs)

# ==============================================================================
# Application identity
#
# LPS_OUTPUT_NAME / LPS_BUNDLE_ID / LPS_DESKTOP_ID are a shared contract: CI,
# the macOS Info.plist, the Windows VERSIONINFO resource, the Linux desktop
# entry and ui/main.cpp all derive from them. Change one, change all of them.
# ==============================================================================
set(LPS_APP_NAME "Lumen Photo Studio" CACHE STRING "Application display name")
set(LPS_APP_VERSION "${PROJECT_VERSION}" CACHE STRING "Application version")
set(LPS_COMPANY_NAME "Lumen" CACHE STRING "Company / organization name")
set(LPS_APP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/lumen_logo_512.png"
    CACHE FILEPATH "Application icon path")
set(LPS_APP_COPYRIGHT "Copyright (c) 2026 Lumen Photo Studio contributors."
    CACHE STRING "Copyright line embedded in platform metadata")

set(LPS_OUTPUT_NAME     "LumenPhotoStudio")
set(LPS_BUNDLE_ID       "studio.lumen.photostudio")
set(LPS_DESKTOP_ID      "studio.lumen.photostudio")
set(LPS_ORG_DOMAIN      "lumen.studio")
set(LPS_APP_DESCRIPTION "${PROJECT_DESCRIPTION}")

# Windows VERSIONINFO needs four integers. LPS_APP_VERSION is a free-form cache
# string, so parse defensively and fall back to zeroes rather than emitting an
# .rc the resource compiler will reject.
set(LPS_VER_MAJOR 0)
set(LPS_VER_MINOR 0)
set(LPS_VER_PATCH 0)
set(LPS_VER_TWEAK 0)
if(LPS_APP_VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)[.]([0-9]+)")
    set(LPS_VER_MAJOR "${CMAKE_MATCH_1}")
    set(LPS_VER_MINOR "${CMAKE_MATCH_2}")
    set(LPS_VER_PATCH "${CMAKE_MATCH_3}")
    set(LPS_VER_TWEAK "${CMAKE_MATCH_4}")
elseif(LPS_APP_VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)")
    set(LPS_VER_MAJOR "${CMAKE_MATCH_1}")
    set(LPS_VER_MINOR "${CMAKE_MATCH_2}")
    set(LPS_VER_PATCH "${CMAKE_MATCH_3}")
elseif(LPS_APP_VERSION MATCHES "^([0-9]+)[.]([0-9]+)")
    set(LPS_VER_MAJOR "${CMAKE_MATCH_1}")
    set(LPS_VER_MINOR "${CMAKE_MATCH_2}")
elseif(LPS_APP_VERSION MATCHES "^([0-9]+)")
    set(LPS_VER_MAJOR "${CMAKE_MATCH_1}")
endif()

# Runtime data (plugin drop-in dir, preset drop-in dir, docs).
#
# On macOS this MUST be bundle-relative. An installed .app is relocatable, so
# anything staged into ${CMAKE_INSTALL_DATADIR} lands in a share/ tree next to
# the bundle that the application can never reach at runtime. Everywhere else
# the GNU layout is correct.
if(APPLE)
    set(LPS_INSTALL_DATA_DIR "${LPS_OUTPUT_NAME}.app/Contents/Resources"
        CACHE STRING "Runtime data directory, relative to CMAKE_INSTALL_PREFIX")
else()
    set(LPS_INSTALL_DATA_DIR "${CMAKE_INSTALL_DATADIR}/LumenPhotoStudio"
        CACHE STRING "Runtime data directory, relative to CMAKE_INSTALL_PREFIX")
endif()

# Warning flags are scoped to CXX explicitly. On Windows the target also
# compiles a .rc file, and rc.exe / windres reject C++ compiler switches; the
# COMPILE_LANGUAGE guard keeps them out of the resource-compiler command line.
if(MSVC)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/W4>"
        "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
    )
else()
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:-Wall>"
        "$<$<COMPILE_LANGUAGE:CXX>:-Wextra>"
        "$<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>"
    )
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Concurrent Widgets)

# ==============================================================================
# Optional LibRaw discovery
#
# Never fatal: without LibRaw the engine simply reports RAW support disabled.
# pkg-config covers Linux and Homebrew; the explicit PATHS cover Homebrew on
# both architectures, MacPorts and a manually-set LIBRAW_ROOT. vcpkg and Conan
# are already handled because their toolchains populate CMAKE_PREFIX_PATH.
# ==============================================================================
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBRAW QUIET libraw)
endif()

find_path(LIBRAW_INCLUDE_DIR
    NAMES libraw/libraw.h
    HINTS
        ${PC_LIBRAW_INCLUDEDIR}
        ${PC_LIBRAW_INCLUDE_DIRS}
        ${LIBRAW_ROOT}
        $ENV{LIBRAW_ROOT}
    PATHS
        /opt/homebrew/include        # Homebrew, Apple Silicon
        /usr/local/include           # Homebrew, Intel / manual installs
        /opt/local/include           # MacPorts
    PATH_SUFFIXES include
)
find_library(LIBRAW_LIBRARY
    NAMES raw libraw raw_r libraw_r
    HINTS
        ${PC_LIBRAW_LIBDIR}
        ${PC_LIBRAW_LIBRARY_DIRS}
        ${LIBRAW_ROOT}
        $ENV{LIBRAW_ROOT}
    PATHS
        /opt/homebrew/lib
        /usr/local/lib
        /opt/local/lib
    PATH_SUFFIXES lib
)
set(LPS_HAS_LIBRAW OFF)
if(LIBRAW_INCLUDE_DIR AND LIBRAW_LIBRARY)
    set(LPS_HAS_LIBRAW ON)
endif()

set(LPS_SOURCES
    # core/
    src/core/Look.h
    src/core/Look.cpp
    src/core/ImagePipeline.h
    src/core/ImagePipeline.cpp
    src/core/PixelBuffer.h
    src/core/PixelBuffer.cpp

    # io/
    src/io/ImageMetadataReader.h
    src/io/ImageMetadataReader.cpp
    src/io/RawImageLoader.h
    src/io/RawImageLoader.cpp

    # hdr/
    src/hdr/HDRToneMapper.h
    src/hdr/HDRToneMapper.cpp

    # tone/
    src/tone/ToneEngine.h
    src/tone/ToneEngine.cpp
    src/tone/Exposure.h
    src/tone/Exposure.cpp
    src/tone/Contrast.h
    src/tone/Contrast.cpp
    src/tone/HighlightsShadows.h
    src/tone/HighlightsShadows.cpp

    # color/
    src/color/ColorEngine.h
    src/color/ColorEngine.cpp
    src/color/WhiteBalance.h
    src/color/WhiteBalance.cpp
    src/color/Vibrance.h
    src/color/Vibrance.cpp
    src/color/HSL.h
    src/color/HSL.cpp
    src/color/RGBMixer.h
    src/color/RGBMixer.cpp

    # curve/
    src/curve/CurveEngine.h
    src/curve/CurveEngine.cpp
    src/curve/ToneCurve.h
    src/curve/ToneCurve.cpp

    # grading/
    src/grading/ColorGrading.h
    src/grading/ColorGrading.cpp
    src/grading/LUTLoader.h
    src/grading/LUTLoader.cpp
    src/grading/FilmProfiles.h
    src/grading/FilmProfiles.cpp

    # details/
    src/details/DetailsEngine.h
    src/details/DetailsEngine.cpp

    # transform/
    src/transform/TransformEngine.h
    src/transform/TransformEngine.cpp

    # effects/
    src/effects/EffectsEngine.h
    src/effects/EffectsEngine.cpp
    src/effects/Vignette.h
    src/effects/Vignette.cpp
    src/effects/Grain.h
    src/effects/Grain.cpp
    src/effects/Clarity.h
    src/effects/Clarity.cpp

    # local/
    src/local/LocalAdjustmentEngine.h
    src/local/LocalAdjustmentEngine.cpp
    src/local/MaskGeometry.h

    # lens/
    src/lens/LensCorrectionEngine.h
    src/lens/LensCorrectionEngine.cpp

    # preset/
    src/preset/PresetManager.h
    src/preset/PresetManager.cpp
    src/preset/LookSerializer.h
    src/preset/LookSerializer.cpp

    # plugins/
    src/plugins/PluginManager.h
    src/plugins/PluginManager.cpp

    # project/
    src/project/AutosaveManager.h
    src/project/AutosaveManager.cpp
    src/project/ProjectDocument.h
    src/project/ProjectDocument.cpp
    src/project/ProjectSerializer.h
    src/project/ProjectSerializer.cpp

    # settings/
    src/settings/SettingsManager.h
    src/settings/SettingsManager.cpp

    # util/ (header-only)
    src/util/ScanlineParallel.h
    src/util/ColorMath.h
    src/util/ColorSpace.h
)

add_library(lps_engine STATIC ${LPS_SOURCES})

target_include_directories(lps_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lps_engine
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Concurrent
)

if(LPS_HAS_LIBRAW)
    target_compile_definitions(lps_engine PUBLIC LPS_HAS_LIBRAW=1)
    target_include_directories(lps_engine PRIVATE ${LIBRAW_INCLUDE_DIR})
    target_link_libraries(lps_engine PRIVATE ${LIBRAW_LIBRARY})
    message(STATUS "LibRaw found: RAW support enabled")
else()
    message(STATUS "LibRaw not found: RAW support disabled")
endif()

# ==============================================================================
# LumenPhotoStudio — Qt Widgets application front-end
#
# Depends on lps_engine (the static library defined above) plus Qt Widgets.
# Auto-MOC is enabled at the target level so we don't need manual moc rules
# for MainWindow's Q_OBJECT macro.
#
# The CMake target name and the OUTPUT_NAME are deliberately identical on every
# platform (LumenPhotoStudio / LumenPhotoStudio.exe / LumenPhotoStudio.app) so
# CI can hardcode the artifact path.
# ==============================================================================
set(LUMEN_UI_SOURCES
    ui/main.cpp
    ui/MainWindow.h
    ui/MainWindow.cpp
    ui/CurveEditorWidget.h
    ui/CurveEditorWidget.cpp
    ui/HistogramWidget.h
    ui/HistogramWidget.cpp
    ui/EmptyStateOverlay.h
    ui/EmptyStateOverlay.cpp
    ui/WelcomeScreenWidget.h
    ui/WelcomeScreenWidget.cpp
    ui/ExportDialog.h
    ui/ExportDialog.cpp
    ui/PreviewWidget.h
    ui/PreviewWidget.cpp
    ui/ColorWheelWidget.h
    ui/ColorWheelWidget.cpp
    ui/NodeGraphWidget.h
    ui/NodeGraphWidget.cpp
    ui/SecondaryViewerWindow.h
    ui/SecondaryViewerWindow.cpp
    resources/resources.qrc
)

# Build-time identity header. ui/main.cpp reads its version, organization
# domain and desktop-file id from here so none of them can drift away from
# project(VERSION) / the identity block above.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/cmake/lps_build_info.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lps_build_info.h"
    @ONLY
)

# ------------------------------------------------------------------------------
# Windows: VERSIONINFO + application icon resource
#
# The .ico is optional. resources/icons/lumen_logo_512.png is the only icon
# asset in the repository, and a real multi-resolution .ico still has to be
# generated by hand (see docs/BUILDING.md). Until packaging/windows/lumen.ico
# exists the ICON statement is omitted entirely rather than pointing the
# resource compiler at a missing file.
# ------------------------------------------------------------------------------
if(WIN32)
    enable_language(RC)

    set(LPS_WIN_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/lumen.ico")
    if(EXISTS "${LPS_WIN_ICON_FILE}")
        set(LPS_WIN_ICON_RC_LINE "1 ICON \"${LPS_WIN_ICON_FILE}\"")
    else()
        set(LPS_WIN_ICON_RC_LINE "/* packaging/windows/lumen.ico absent - no ICON resource. */")
        message(STATUS "packaging/windows/lumen.ico not found: the .exe will use the default Windows icon")
    endif()

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/app.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/windows/app.rc"
        @ONLY
    )
    list(APPEND LUMEN_UI_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/packaging/windows/app.rc")
endif()

qt_add_executable(LumenPhotoStudio ${LUMEN_UI_SOURCES})

set_target_properties(LumenPhotoStudio PROPERTIES
    OUTPUT_NAME       "${LPS_OUTPUT_NAME}"
    AUTOMOC ON
    AUTORCC ON
    WIN32_EXECUTABLE  ON   # no console window on Windows
    MACOSX_BUNDLE     ON   # .app bundle on macOS
)

target_include_directories(LumenPhotoStudio
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(LumenPhotoStudio
    PRIVATE
        lps_engine
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

# ------------------------------------------------------------------------------
# macOS: bundle metadata
#
# The custom Info.plist is configured here (rather than relying on CMake's
# implicit MACOSX_BUNDLE_* substitution) so we can also declare
# NSHighResolutionCapable, NSRequiresAquaSystemAppearance and the document
# types the app can open. The individual MACOSX_BUNDLE_* properties are still
# set because other tooling (macdeployqt, CPack) reads them.
#
# CFBundleIconFile stays empty until packaging/macos/LumenPhotoStudio.icns is
# generated by hand; see docs/BUILDING.md.
# ------------------------------------------------------------------------------
if(APPLE)
    set(LPS_BUNDLE_EXECUTABLE "${LPS_OUTPUT_NAME}")
    set(LPS_MACOS_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/${LPS_OUTPUT_NAME}.icns")

    if(EXISTS "${LPS_MACOS_ICNS}")
        set(LPS_BUNDLE_ICON_FILE "${LPS_OUTPUT_NAME}.icns")
    else()
        set(LPS_BUNDLE_ICON_FILE "")
        message(STATUS "packaging/macos/${LPS_OUTPUT_NAME}.icns not found: the bundle will use the default macOS icon")
    endif()

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/macos/Info.plist"
        @ONLY
    )

    set_target_properties(LumenPhotoStudio PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST           "${CMAKE_CURRENT_BINARY_DIR}/packaging/macos/Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER       "${LPS_BUNDLE_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME          "${LPS_APP_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION       "${LPS_APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${LPS_APP_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT            "${LPS_APP_COPYRIGHT}"
        MACOSX_BUNDLE_ICON_FILE            "${LPS_BUNDLE_ICON_FILE}"
    )

    if(EXISTS "${LPS_MACOS_ICNS}")
        target_sources(LumenPhotoStudio PRIVATE "${LPS_MACOS_ICNS}")
        set_source_files_properties("${LPS_MACOS_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
    endif()
endif()

# ==============================================================================
# Install / packaging foundation
# ==============================================================================
file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/package-placeholders/plugins"
    "${CMAKE_CURRENT_BINARY_DIR}/package-placeholders/presets"
)

install(TARGETS LumenPhotoStudio
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Qt resources are compiled into the executable through resources.qrc.
# Runtime extension points are installed as writable placeholders.
# LPS_INSTALL_DATA_DIR resolves inside the .app bundle on macOS.
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package-placeholders/plugins/"
    DESTINATION "${LPS_INSTALL_DATA_DIR}/plugins"
)
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package-placeholders/presets/"
    DESTINATION "${LPS_INSTALL_DATA_DIR}/presets"
)

install(FILES
        docs/BUILDING.md
        docs/DEPLOYMENT.md
    DESTINATION "${LPS_INSTALL_DATA_DIR}/docs"
)

# ------------------------------------------------------------------------------
# Linux / *BSD desktop integration
#
# Guarded on UNIX-and-not-Apple: on macOS the equivalent metadata lives in the
# bundle's Info.plist, and on Windows in the VERSIONINFO resource.
# ------------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/${LPS_DESKTOP_ID}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )

    install(FILES "${LPS_APP_ICON_PATH}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps"
        RENAME "${LPS_DESKTOP_ID}.png"
    )

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/${LPS_DESKTOP_ID}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
    )
endif()
