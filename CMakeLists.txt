cmake_minimum_required(VERSION 3.20)

project(vixen LANGUAGES CXX)

add_executable(vixen
    src/main.cpp

    third_party/nvvfx/src/nvCVImageProxy.cpp
    third_party/nvvfx/src/NVVideoEffectsProxy.cpp
)

target_include_directories(vixen PRIVATE
    third_party/nvvfx/include
)

target_compile_features(vixen PRIVATE cxx_std_17)

if(MINGW)
    target_compile_options(vixen PRIVATE
        -Wall
        -Wextra
    )

    target_link_options(vixen PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

