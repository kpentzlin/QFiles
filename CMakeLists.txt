cmake_minimum_required(VERSION 3.20)

project(QFiles VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "QFiles wird ausschließlich als 64-Bit-Anwendung gebaut.")
endif()

# ---------------------------------------------------------------------------
# libssh2 (SFTP) – statisch, mit Windows-Kryptografie (WinCNG), siehe third_party/libssh2/README.QFiles.md
set(LIBSSH2_DIR ${CMAKE_SOURCE_DIR}/third_party/libssh2)
set(LIBSSH2_SOURCES agent.c bcrypt_pbkdf.c channel.c comp.c chacha.c cipher-chachapoly.c crypt.c crypto.c global.c
    hostkey.c keepalive.c kex.c knownhost.c mac.c misc.c packet.c pem.c poly1305.c publickey.c scp.c session.c sftp.c
    transport.c userauth.c userauth_kbd_packet.c version.c)
list(TRANSFORM LIBSSH2_SOURCES PREPEND ${LIBSSH2_DIR}/src/)
add_library(qf_libssh2 STATIC ${LIBSSH2_SOURCES})
set_target_properties(qf_libssh2 PROPERTIES LINKER_LANGUAGE C)
target_include_directories(qf_libssh2 PUBLIC ${LIBSSH2_DIR}/include PRIVATE ${LIBSSH2_DIR}/src)
target_compile_definitions(qf_libssh2 PRIVATE LIBSSH2_WINCNG LIBSSH2_ECDSA_WINCNG LIBSSH2_LIBRARY
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(qf_libssh2 PUBLIC ws2_32 bcrypt crypt32)
if(MSVC)
    target_compile_options(qf_libssh2 PRIVATE /W1 /utf-8)
    set_property(TARGET qf_libssh2 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(qf_libssh2 PRIVATE -w)
endif()

# Build-Kennung (Commit) für den Info-Dialog, damit sich Versionen unterscheiden lassen
set(QFILES_BUILD_ID "")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE QFILES_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
string(TIMESTAMP QFILES_BUILD_DATE "%Y-%m-%d")

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
    QFILES_BUILD_ID="${QFILES_BUILD_ID}"
    QFILES_BUILD_DATE="${QFILES_BUILD_DATE}"
)

target_link_libraries(QFiles PRIVATE
    comctl32 shell32 shlwapi ole32 oleaut32 uuid
    gdi32 user32 comdlg32 advapi32 windowscodecs propsys version msimg32 uxtheme dbghelp bcrypt
    qf_libssh2 ws2_32 crypt32 netapi32 mpr winhttp
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
        src/Location.cpp src/RemoteCommon.cpp src/FtpClient.cpp src/SftpClient.cpp src/WebDavClient.cpp
    )
    target_include_directories(QFilesTests PRIVATE src res)
    target_compile_definitions(QFilesTests PRIVATE UNICODE _UNICODE NOMINMAX STRICT _WIN32_WINNT=0x0A00 WINVER=0x0A00
        NTDDI_VERSION=0x0A000006 QFILES_VERSION_STRING="${PROJECT_VERSION}")
    target_link_libraries(QFilesTests PRIVATE comctl32 shell32 shlwapi ole32 oleaut32 uuid gdi32 user32 comdlg32
        advapi32 windowscodecs propsys qf_libssh2 ws2_32 crypt32 winhttp)
    if(MSVC)
        target_compile_options(QFilesTests PRIVATE /utf-8 /EHsc /permissive-)
        set_property(TARGET QFilesTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(QFilesTests PRIVATE -municode -static -static-libgcc -static-libstdc++)
    endif()

    # Test-Kontextmenü-Erweiterung für den GUI-Test (Ausschluss von Erweiterungen)
    add_library(QFilesTestExt SHARED tests/TestShellExt.cpp tests/TestShellExt.def)
    target_compile_definitions(QFilesTestExt PRIVATE UNICODE _UNICODE NOMINMAX STRICT _WIN32_WINNT=0x0A00)
    target_link_libraries(QFilesTestExt PRIVATE ole32 uuid user32 shell32)
    set_target_properties(QFilesTestExt PROPERTIES PREFIX "")
    if(MSVC)
        target_compile_options(QFilesTestExt PRIVATE /utf-8 /EHsc /permissive-)
        set_property(TARGET QFilesTestExt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(QFilesTestExt PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
