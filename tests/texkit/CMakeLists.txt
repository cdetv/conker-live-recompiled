# Texture pack kit test (to-do 238): src/texkit.c's pieces alone, no game data.
# In a VS dev shell:
#   cmake -S tests/texkit -B _work/texkit-test -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build _work/texkit-test && ctest --test-dir _work/texkit-test --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(texkit_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(texkit_test texkit_test.c ../../src/texkit.c ../../src/texkit_pixels.c
               ../../src/png_write.c ../../src/deflate.c ../../src/caff.c)
target_include_directories(texkit_test PRIVATE ../../src)
target_compile_definitions(texkit_test PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(texkit_test PRIVATE /W4)
endif()

enable_testing()
add_test(NAME texkit COMMAND texkit_test)
