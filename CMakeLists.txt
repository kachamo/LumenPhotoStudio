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

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Concurrent Widgets)

set(LPS_SOURCES
    # core/
    src/core/Look.h
    src/core/Look.cpp
    src/core/ImagePipeline.h
    src/core/ImagePipeline.cpp
    src/core/PixelBuffer.h
    src/core/PixelBuffer.cpp

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

    # effects/
    src/effects/EffectsEngine.h
    src/effects/EffectsEngine.cpp
    src/effects/Vignette.h
    src/effects/Vignette.cpp
    src/effects/Grain.h
    src/effects/Grain.cpp
    src/effects/Clarity.h
    src/effects/Clarity.cpp

    # preset/
    src/preset/PresetManager.h
    src/preset/PresetManager.cpp
    src/preset/LookSerializer.h
    src/preset/LookSerializer.cpp

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

# ==============================================================================
# LumenPhotoUI — minimal Qt Widgets test UI for the engine
#
# Depends on lps_engine (the static library defined above) plus Qt Widgets.
# Auto-MOC is enabled at the target level so we don't need manual moc rules
# for MainWindow's Q_OBJECT macro.
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
    ui/PreviewWidget.h
    ui/PreviewWidget.cpp
    ui/SecondaryViewerWindow.h
    ui/SecondaryViewerWindow.cpp
    resources/resources.qrc
)

qt_add_executable(LumenPhotoUI ${LUMEN_UI_SOURCES})

set_target_properties(LumenPhotoUI PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    WIN32_EXECUTABLE  ON   # no console window on Windows
    MACOSX_BUNDLE     ON   # .app bundle on macOS
)

target_link_libraries(LumenPhotoUI
    PRIVATE
        lps_engine
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
