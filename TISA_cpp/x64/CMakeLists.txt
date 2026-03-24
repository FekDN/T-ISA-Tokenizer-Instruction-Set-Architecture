cmake_minimum_required(VERSION 3.16)
project(tisa_test_x64 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Per-compiler warning flags ────────────────────────────────────────────────
if(MSVC)
    add_compile_options(
        /W3 /wd4100 /wd4127 /wd4244 /wd4267 /wd4996 /utf-8
    )
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-sign-compare
    )
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
add_executable(tisa_test_x64 tisa_test_x64.cpp)

target_include_directories(tisa_test_x64 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ── Linking ───────────────────────────────────────────────────────────────────
if(MSVC)
    # /MT  = embed CRT statically (no MSVCRT*.dll dependency)
    # /O2  = optimise for speed
    # /GL  = whole-program optimisation
    target_compile_options(tisa_test_x64 PRIVATE
        $<$<CONFIG:Release>:/O2 /GL /DNDEBUG /MT>
        $<$<CONFIG:Debug>:/Od /Zi /MTd>
    )
    target_link_options(tisa_test_x64 PRIVATE
        $<$<CONFIG:Release>:/LTCG>
    )

elseif(WIN32)
    # MinGW — embed libstdc++, libgcc, libpthread into the .exe
    # so tisa_test_x64.exe runs on any Windows machine without DLL side-load.
    target_compile_options(tisa_test_x64 PRIVATE
        $<$<CONFIG:Release>:-O3 -march=native -funroll-loops -DNDEBUG>
        $<$<CONFIG:Debug>:-O0 -g3>
    )
    target_link_options(tisa_test_x64 PRIVATE
        -static
        -static-libgcc
    )
    target_link_libraries(tisa_test_x64 PRIVATE
        -Wl,-Bstatic
        stdc++
        gcc
        pthread
        -Wl,-Bdynamic
    )

else()
    # Linux / macOS — glibc stays dynamic (correct); pthreads via CMake
    find_package(Threads REQUIRED)
    target_link_libraries(tisa_test_x64 PRIVATE Threads::Threads)
    target_compile_options(tisa_test_x64 PRIVATE
        $<$<CONFIG:Release>:-O3 -march=native -funroll-loops -DNDEBUG>
        $<$<CONFIG:Debug>:-O0 -g3 -fsanitize=address,undefined>
    )
    target_link_options(tisa_test_x64 PRIVATE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS tisa_test_x64 RUNTIME DESTINATION bin)

# ── Usage hint ────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Output     : ${CMAKE_BINARY_DIR}")
message(STATUS "")
message(STATUS "  Run:")
message(STATUS "    tisa_test_x64                         (default paths)")
message(STATUS "    tisa_test_x64 <models_dir> <suite>   (custom paths)")
message(STATUS "")
message(STATUS "  Expected layout:")
message(STATUS "    models/model_map.txt")
message(STATUS "    models/<hash>/vocab.b")
message(STATUS "    models/<hash>/vocab_idx.b")
message(STATUS "    models/<hash>/merges.b     (BPE only)")
message(STATUS "    tisa_test_suite.bin")
message(STATUS "")
