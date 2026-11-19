# Package reader test (to-do 43): src/caff.c alone, no game. In a VS dev shell:
#   cmake -S tests/caff -B _work/caff-test -G Ninja -DCMAKE_BUILD_TYPE=Release [-DCAFF_ASAN=ON]
#   cmake --build _work/caff-test
# Usage in caff_test.c's header. It reads the player's game files; its output
# stays in _work (SPEC 3.3).
cmake_minimum_required(VERSION 3.20)
project(caff_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(CAFF_ASAN "Build with AddressSanitizer (catches reads past a buffer)" OFF)

add_executable(caff_test caff_test.c ../../src/caff.c ../../src/caff_inventory.c)
target_include_directories(caff_test PRIVATE ../../src)
target_compile_definitions(caff_test PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(caff_test PRIVATE /W4)
    if(CAFF_ASAN)
        target_compile_options(caff_test PRIVATE /fsanitize=address /Zi)
        target_link_options(caff_test PRIVATE /DEBUG)
    endif()
endif()
