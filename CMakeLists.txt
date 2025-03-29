project(uw)

set(CMAKE_C_COMPILER clang-16)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -pedantic -Werror -Wno-gnu -Wno-unused-parameter -Wno-format-pedantic)

if(DEFINED ENV{DEBUG})
    add_compile_options(-g)
else()
    add_compile_options(-O2)
endif()

add_library(amw STATIC
    amw_status.c
    amw_parser.c
    amw_json.c
)

target_include_directories(amw PUBLIC . uw/include libpussy)
