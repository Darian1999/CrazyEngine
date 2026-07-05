cmake_minimum_required(VERSION 3.16)
project(CrazyEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find packages
find_package(glfw3 3.3 REQUIRED)
find_package(OpenGL REQUIRED)

# GLAD (already in project)
add_library(glad STATIC src/glad.c)
target_include_directories(glad PUBLIC include)

# CrazyEngine library
add_library(CrazyEngine STATIC
    src/engine.cpp
    src/esl.cpp
)
target_include_directories(CrazyEngine PUBLIC include)
target_link_libraries(CrazyEngine PUBLIC glad glfw OpenGL::GL ${CMAKE_DL_LIBS})

# Demo executable
add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE CrazyEngine)

# Copy shaders to build directory
add_custom_command(TARGET demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:demo>/shaders
)

# ---------------------------------------------------------------
# Doc-driven test: runs every cookbook/tutorial example from
# docs/ESL.md through crazy::esl::transpile and verifies the
# generated GLSL is well-formed. Catches stale doc examples.
# ---------------------------------------------------------------
add_executable(esl_cookbook_test tests/esl_cookbook_test.cpp)
target_link_libraries(esl_cookbook_test PRIVATE CrazyEngine)
target_compile_definitions(esl_cookbook_test PRIVATE
    DOC_PATH="${CMAKE_SOURCE_DIR}/docs/ESL.md"
)
enable_testing()
add_test(NAME esl_cookbook_test COMMAND esl_cookbook_test)
