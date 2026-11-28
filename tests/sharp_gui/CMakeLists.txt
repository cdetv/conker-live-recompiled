# Sharper text and buttons test (to-do 53 step 5): src/sharp_gui.c's clean-up
# against the prototype scripts' results on made-up pictures (expected.h).
# In a VS dev shell:
#   cmake -S tests/sharp_gui -B _work/sharp-test -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build _work/sharp-test && ctest --test-dir _work/sharp-test --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(sharp_gui_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(sharp_gui_test sharp_gui_test.c ../../src/sharp_gui.c ../../src/texkit.c
               ../../src/texkit_pixels.c ../../src/png_write.c ../../src/deflate.c ../../src/caff.c)
target_include_directories(sharp_gui_test PRIVATE ../../src)
target_compile_definitions(sharp_gui_test PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(sharp_gui_test PRIVATE /W4)
endif()

enable_testing()
add_test(NAME sharp_gui COMMAND sharp_gui_test)
