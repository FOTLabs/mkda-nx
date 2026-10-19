# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
)
if(NINTENDO_SWITCH)
    # The Switch's controllers, read through libnx and mapped by position.
    target_sources(xbox_input PRIVATE xinput_nx.c)
endif()

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
    # xinput_device.c's keyboard stand-in reads xbox_FramebufferKeyDown
    # (video/fb_present.c). Said here so the static link order holds without
    # a renderer happening to pull xbox_video in again later (the Vulkan
    # build did not).
    if(NOT NINTENDO_SWITCH)
        target_link_libraries(xbox_input PUBLIC xbox_video)
    endif()
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
