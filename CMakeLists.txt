cmake_minimum_required(VERSION 3.21)

project(DivaFineWindow VERSION 1.0.0 LANGUAGES CXX)

add_library(DivaFineWindow SHARED
    src/dllmain.cpp
)

target_compile_features(DivaFineWindow PRIVATE cxx_std_20)
target_compile_definitions(DivaFineWindow PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(DivaFineWindow PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

set_target_properties(DivaFineWindow PROPERTIES
    OUTPUT_NAME "DivaFineWindow"
    PREFIX ""
)

