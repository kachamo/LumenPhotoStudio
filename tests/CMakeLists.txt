# ==============================================================================
# tests/
#
# Unit tests for the engine. Qt Test is used rather than an external framework
# because it ships inside qtbase — the CI Qt install already has it, so the
# suite costs no new dependency on any platform.
#
# Only lps_engine is under test. The UI is not: driving QWidget code needs a
# platform plugin and a font database, which makes it slow and flaky in CI.
# `--smoke-test` already covers "the window builds and the catalog opens" on
# all three platforms, which is the part of the UI worth guarding here.
# ==============================================================================

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

# Each test is one executable and one CTest entry.
function(lps_add_test name)
    qt_add_executable(${name} ${name}.cpp)
    set_target_properties(${name} PROPERTIES
        AUTOMOC ON
        WIN32_EXECUTABLE OFF   # console app: CTest needs stdout and an exit code
        MACOSX_BUNDLE    OFF   # a .app bundle would hide the binary from CTest
    )
    target_link_libraries(${name} PRIVATE lps_engine Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    # Tests construct QGuiApplication-free code, but Qt still wants a platform
    # plugin the moment anything touches QImage plugins or QPixmap.
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 300
    )
endfunction()

lps_add_test(tst_colorspace)
lps_add_test(tst_pixelbuffer)
lps_add_test(tst_imagepipeline)
lps_add_test(tst_catalogdatabase)
lps_add_test(tst_thumbnailcache)
lps_add_test(tst_catalogimporter)
lps_add_test(tst_lookserializer)
