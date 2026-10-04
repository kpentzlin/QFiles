cmake_minimum_required(VERSION 3.20)

project(QFiles VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "QFiles wird ausschließlich als 64-Bit-Anwendung gebaut.")
endif()

file(GLOB QFILES_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB QFILES_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.h)

add_executable(QFiles WIN32
    ${QFILES_SOURCES}
    ${QFILES_HEADERS}
    res/QFiles.rc
)

target_include_directories(QFiles PRIVATE src res)

target_compile_definitions(QFiles PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    STRICT
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000006
    QFILES_VERSION_STRING="${PROJECT_VERSION}"
)

target_link_libraries(QFiles PRIVATE
    comctl32 shell32 shlwapi ole32 oleaut32 uuid
    gdi32 user32 comdlg32 advapi32 windowscodecs propsys version msimg32 uxtheme dbghelp bcrypt
)

if(MSVC)
    target_compile_options(QFiles PRIVATE /W4 /utf-8 /permissive- /EHsc /MP
        /wd4100 /wd4201 /wd4245 /wd4267 /wd4244 /wd4389)
    # Manifest kommt aus der Ressourcendatei. PDB auch im Release (für Absturzberichte mit Funktionsnamen).
    target_compile_options(QFiles PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(QFiles PRIVATE /MANIFEST:NO $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>)
    set_property(TARGET QFiles PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(QFiles PRIVATE -Wall -Wextra -Wno-unused-parameter
        -Wno-missing-field-initializers -Wno-cast-function-type -Wno-sign-compare)
    target_link_options(QFiles PRIVATE -municode -mwindows -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(QFiles PROPERTIES OUTPUT_NAME "QFiles")

# ---------------------------------------------------------------------------
# Automatische Tests der Kernfunktionen (Konsolenprogramm)
option(QFILES_BUILD_TESTS "Tests bauen" ON)
if(QFILES_BUILD_TESTS)
    add_executable(QFilesTests
        tests/QFilesTests.cpp
        src/Util.cpp src/Dialog.cpp src/Settings.cpp src/App.cpp src/Encoding.cpp src/FileOps.cpp
    )
    target_include_directories(QFilesTests PRIVATE src res)
    target_compile_definitions(QFilesTests PRIVATE UNICODE _UNICODE NOMINMAX STRICT _WIN32_WINNT=0x0A00 WINVER=0x0A00
        NTDDI_VERSION=0x0A000006 QFILES_VERSION_STRING="${PROJECT_VERSION}")
    target_link_libraries(QFilesTests PRIVATE comctl32 shell32 shlwapi ole32 oleaut32 uuid gdi32 user32 comdlg32
        advapi32 windowscodecs propsys)
    if(MSVC)
        target_compile_options(QFilesTests PRIVATE /utf-8 /EHsc /permissive-)
        set_property(TARGET QFilesTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(QFilesTests PRIVATE -municode -static -static-libgcc -static-libstdc++)
    endif()
endif()
