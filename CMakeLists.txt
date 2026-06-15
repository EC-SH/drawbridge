cmake_minimum_required (VERSION 3.16)

if(DEFINED ENV{IDF_PATH})
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project("SipServer")
    idf_build_set_property(COMPILE_DEFINITIONS "LV_TICK_CUSTOM_SYS_TIME_EXPR=((esp_timer_get_time()/1000LL))" APPEND)
else()
    project ("SipServer")

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    set(SRC_DIR "${PROJECT_SOURCE_DIR}/src")

    set(HELPERS_SRC_FILES "${SRC_DIR}/Helpers")
    file(GLOB_RECURSE HELPERS_SRC
        "${HELPERS_SRC_FILES}/*.cpp"
        "${HELPERS_SRC_FILES}/*.h"
        "${HELPERS_SRC_FILES}/*.hpp"
    )

    set(SIP_SRC_FILES "${SRC_DIR}/SIP")
    file(GLOB_RECURSE SIP_SRC
        "${SIP_SRC_FILES}/*.cpp"
        "${SIP_SRC_FILES}/*.h"
        "${SIP_SRC_FILES}/*.hpp"
    )

    include_directories(${HELPERS_SRC_FILES} ${SIP_SRC_FILES})
    add_executable (SipServer "main.cpp" ${SIP_SRC} ${HELPERS_SRC} )
    if (WIN32)
        target_link_libraries(SipServer wsock32 ws2_32)
    elseif (UNIX)
        # 64-bit std::atomic counters (RequestsHandler packet stats) lower to libatomic
        # calls on some 32-bit targets — link it. Harmless on x86-64 where they inline.
        target_link_libraries(SipServer -pthread atomic)
    else()
        message(FATAL_ERROR "Unsupported platform.")
    endif()

    target_compile_options(SipServer PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )

    source_group("Helpers"  FILES ${HELPERS_SRC_FILES})
    source_group("SIP"      FILES ${SIP_SRC_FILES})

    # ── Optional: SSH sysop terminal in the host build ─────────────────────────
    # PD_HOST_SSH=ON links wolfSSL (FetchContent, its own CMake) and compiles the
    # wolfSSH sources directly into a small static lib (its build system is
    # autotools-first; vendoring the build, not the code). The same
    # cmake/patch_wolfssl.py pty-req fixes used for the ESP managed component are
    # re-applied to the fetched wolfSSH tree — OpenSSH/PuTTY clients send the
    # same empty terminal-modes string that trips the unpatched parser.
    #   cmake ... -DPD_HOST_SSH=ON [-DPD_HOST_SSH_PORT=2222]
    option(PD_HOST_SSH "Serve the SSH sysop-terminal TUI from the host binary" OFF)
    if(PD_HOST_SSH)
        if(NOT DEFINED PD_HOST_SSH_PORT)
            set(PD_HOST_SSH_PORT 2222)   # 22 is usually taken/privileged on desktops
        endif()
        include(FetchContent)
        set(WOLFSSL_WOLFSSH    ON  CACHE INTERNAL "")
        set(WOLFSSL_EXAMPLES   OFF CACHE INTERNAL "")
        set(WOLFSSL_CRYPT_TESTS OFF CACHE INTERNAL "")
        # Keys come from in-memory buffers (certs_test.h bundled host key).
        # Filesystem OFF makes wolfssl's generated options.h DEFINE NO_FILESYSTEM
        # instead of #undef-ing it — which would silently disable that key block
        # (options.h is included by every wolfssh header via settings.h).
        set(WOLFSSL_FILESYSTEM OFF CACHE INTERNAL "")
        FetchContent_Declare(wolfssl
            GIT_REPOSITORY https://github.com/wolfSSL/wolfssl.git
            GIT_TAG        v5.8.2-stable
            GIT_SHALLOW    TRUE)
        # SOURCE_SUBDIR points at a dir with no CMakeLists so MakeAvailable
        # populates the sources without add_subdirectory()-ing its build.
        FetchContent_Declare(wolfssh
            GIT_REPOSITORY https://github.com/wolfSSL/wolfssh.git
            GIT_TAG        v1.4.20-stable
            GIT_SHALLOW    TRUE
            SOURCE_SUBDIR  do-not-add-subdir)
        FetchContent_MakeAvailable(wolfssl wolfssh)

        # Re-apply the wolfSSH pty-req fixes (see the script header, fixes 4 & 5).
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/cmake/patch_wolfssl.py"
                    "${FETCHCONTENT_BASE_DIR}"
            RESULT_VARIABLE _ssh_patch_rc)
        if(NOT _ssh_patch_rc EQUAL 0)
            message(FATAL_ERROR "patch_wolfssl.py failed (rc=${_ssh_patch_rc}) on the FetchContent tree")
        endif()

        add_library(wolfssh_host STATIC
            "${wolfssh_SOURCE_DIR}/src/ssh.c"
            "${wolfssh_SOURCE_DIR}/src/internal.c"
            "${wolfssh_SOURCE_DIR}/src/io.c"
            "${wolfssh_SOURCE_DIR}/src/log.c"
            "${wolfssh_SOURCE_DIR}/src/port.c"
            "${wolfssh_SOURCE_DIR}/src/keygen.c")
        target_include_directories(wolfssh_host PUBLIC "${wolfssh_SOURCE_DIR}")
        # PUBLIC: SshServer.cpp includes wolfssh/internal.h and must see the SAME
        # config as the lib or struct layouts diverge.
        #   WOLFSSH_TERM    — pty-req support (widthChar/heightRows on WOLFSSH)
        #   NO_FILESYSTEM   — keys come from buffers; also gates certs_test.h's
        #                     bundled host key that SshServer falls back to
        target_compile_definitions(wolfssh_host PUBLIC WOLFSSH_TERM NO_FILESYSTEM)
        target_link_libraries(wolfssh_host PUBLIC wolfssl)

        target_compile_definitions(SipServer PRIVATE
            POCKETDIAL_HAS_WOLFSSH
            POCKETDIAL_SSH_PORT=${PD_HOST_SSH_PORT})
        target_link_libraries(SipServer wolfssh_host)
    endif()

    # Tests pull googletest in via FetchContent — skip with -DBUILD_TESTING=OFF.
    # NOTE: the project is going ESP32-only (see #96). The host build now exists ONLY as
    # the unit-test / cppcheck gate (pending the on-target QEMU port); the shipped desktop
    # binary and the ARMv7/Orbic Linux cross-compile (#82) were removed — the Orbic port
    # is archived at GlomarGadaffi/drawbridge-orbic.
    option(BUILD_TESTING "Build the host unit tests" ON)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
