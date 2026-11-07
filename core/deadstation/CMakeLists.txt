# Deadstation Portable's frontend: runs the core on the Brick, drawing through Vulkan to fb0.
find_package(SDL2 REQUIRED)

add_executable(deadstation-portable main.cpp audio.cpp audio.h clock.cpp clock.h controls.cpp controls.h dead_ui.h log.h menu.cpp menu.h osd.cpp osd.h
		panel.cpp panel.h jitcache.cpp jitcache.h profiler.cpp profiler.h settings.cpp settings.h states.cpp states.h types.h)
target_compile_definitions(deadstation-portable PRIVATE DEADSTATION_VERSION="${DEADSTATION_VERSION}" DEADSTATION_APP_VERSION="${PROJECT_VERSION}")
target_compile_options(deadstation-portable PRIVATE -O2 -Wall -Wno-everything)
target_link_libraries(deadstation-portable PRIVATE deadstation_core deadui SDL2::SDL2 ZLIB::ZLIB)
