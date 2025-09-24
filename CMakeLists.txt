cmake_minimum_required(VERSION 3.20)
project(VDM_ImGui)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Sources
file(GLOB IMGUI_SRC "src/thirdparty/imgui/*.cpp")
file(GLOB APP_SRC "src/*.cpp")

# Create executable
add_executable(VDM_ImGui ${APP_SRC} ${IMGUI_SRC})

# Include directories
target_include_directories(VDM_ImGui PRIVATE
    src/thirdparty/imgui
    src/thirdparty/imgui/backends
    src
)

# Find required packages
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

# Link libraries
target_link_libraries(VDM_ImGui 
    glfw 
    OpenGL::GL
)

# Windows-specific libraries for folder browser
if(WIN32)
    target_link_libraries(VDM_ImGui 
        shell32
        ole32
        comdlg32
    )
    
    # Set Windows subsystem for release builds
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_target_properties(VDM_ImGui PROPERTIES
            WIN32_EXECUTABLE TRUE
        )
    endif()
endif()

# Compiler-specific options
if(MSVC)
    target_compile_options(VDM_ImGui PRIVATE /W4)
    # Disable specific warnings that ImGui might trigger
    target_compile_options(VDM_ImGui PRIVATE /wd4996) # Disable deprecated function warnings
else()
    target_compile_options(VDM_ImGui PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Set output directory
set_target_properties(VDM_ImGui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Copy required files to output directory (if you have any config files, etc.)
# file(COPY ${CMAKE_SOURCE_DIR}/config DESTINATION ${CMAKE_BINARY_DIR}/bin)

# For debugging: print some information
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")

# Optional: Create a custom target to copy yt-dlp and ffmpeg binaries
# if they're included in your project
if(EXISTS "${CMAKE_SOURCE_DIR}/src/thirdparty/bin")
    add_custom_command(TARGET VDM_ImGui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/src/thirdparty/bin
        $<TARGET_FILE_DIR:VDM_ImGui>/src/thirdparty/bin
        COMMENT "Copying third-party binaries"
    )
endif()