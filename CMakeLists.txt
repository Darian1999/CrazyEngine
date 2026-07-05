cmake_minimum_required(VERSION 3.16)
project(CrazyEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------
# Bullet Physics (pin 3.25, slim build — no demos, no unit tests,
# no extras, no install, no soft body). Pulled in via FetchContent
# so users don't need a system-installed bullet package.
# ---------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    bullet3
    GIT_REPOSITORY https://github.com/bulletphysics/bullet3.git
    GIT_TAG        3.25
)
# Bullet's options must be set before its CMakeLists.txt is processed.
set(BUILD_UNIT_TESTS                       OFF CACHE BOOL "" FORCE)
set(BUILD_CPU_DEMOS                        OFF CACHE BOOL "" FORCE)
set(BUILD_BULLET2_DEMOS                    OFF CACHE BOOL "" FORCE)
set(BUILD_OPENGL3_DEMOS                    OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS                           OFF CACHE BOOL "" FORCE)
set(BUILD_PYBULLET                         OFF CACHE BOOL "" FORCE)
set(BUILD_VR_BULLET                        OFF CACHE BOOL "" FORCE)
set(BUILD_CARS                             OFF CACHE BOOL "" FORCE)
set(USE_SOFT_BODY_MULTI_BODY_DYNAMICS_WORLD OFF CACHE BOOL "" FORCE)
set(USE_DOUBLE_PRECISION                   OFF CACHE BOOL "" FORCE)
set(USE_MSVC_2010_COMPILE_FIX              ON  CACHE BOOL "" FORCE)
set(INSTALL_LIBS                           OFF CACHE BOOL "" FORCE)

# Pull bullet3 onto disk, then patch its top-level CMakeLists.txt before
# add_subdirectory. The shipped CMakeLists still asks for
# `cmake_minimum_required(VERSION 2.4)` (or similar), and modern CMake
# refuses to honor anything older than 3.5. We bump it to 3.16 in place;
# nothing else about bullet's build changes.
FetchContent_GetProperties(bullet3)
if(NOT bullet3_POPULATED)
    FetchContent_Populate(bullet3)
endif()
file(READ "${bullet3_SOURCE_DIR}/CMakeLists.txt" _crazy_b3_cmake)
if(_crazy_b3_cmake MATCHES "cmake_minimum_required\\(VERSION[^\\n]*\\)")
    string(REGEX REPLACE
        "cmake_minimum_required\\(VERSION[^\\n]*\\)"
        "cmake_minimum_required(VERSION 3.16)"
        _crazy_b3_cmake "${_crazy_b3_cmake}")
    file(WRITE "${bullet3_SOURCE_DIR}/CMakeLists.txt" "${_crazy_b3_cmake}")
    message(STATUS "[CrazyEngine] Patched bullet3 CMakeLists.txt minimum to 3.16")
endif()
add_subdirectory(${bullet3_SOURCE_DIR} ${bullet3_BINARY_DIR})

# Find packages
find_package(glfw3 3.3 REQUIRED)
find_package(OpenGL REQUIRED)

# GLAD (already in project)
add_library(glad STATIC src/glad.c)
target_include_directories(glad PUBLIC include)

# CrazyEngine library — now includes the physics bindings
add_library(CrazyEngine STATIC
    src/engine.cpp
    src/esl.cpp
    src/physics.cpp
)
target_include_directories(CrazyEngine PUBLIC include)
target_link_libraries(CrazyEngine PUBLIC
    glad glfw OpenGL::GL ${CMAKE_DL_LIBS}
    BulletDynamics BulletCollision LinearMath
)

# Treat third-party Bullet headers as SYSTEM includes so its internal
# warnings don't pollute our build output. CMake 3.16 didn't accept
# multi-var `unset(...)` yet, so each target is unset separately.
get_target_property(_dyn_incs BulletDynamics INCLUDE_DIRECTORIES)
target_include_directories(CrazyEngine SYSTEM PUBLIC ${_dyn_incs})
get_target_property(_col_incs BulletCollision INCLUDE_DIRECTORIES)
target_include_directories(CrazyEngine SYSTEM PUBLIC ${_col_incs})
get_target_property(_lin_incs LinearMath      INCLUDE_DIRECTORIES)
target_include_directories(CrazyEngine SYSTEM PUBLIC ${_lin_incs})
unset(_dyn_incs)
unset(_col_incs)
unset(_lin_incs)

# Demo executable
add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE CrazyEngine)

# Copy shaders to build directory
add_custom_command(TARGET demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:demo>/shaders
)

enable_testing()

# ---------------------------------------------------------------
# Doc-driven test: ESL cookbook
# ---------------------------------------------------------------
add_executable(esl_cookbook_test tests/esl_cookbook_test.cpp)
target_link_libraries(esl_cookbook_test PRIVATE CrazyEngine)
target_compile_definitions(esl_cookbook_test PRIVATE
    DOC_PATH="${CMAKE_SOURCE_DIR}/docs/ESL.md"
)
add_test(NAME esl_cookbook_test COMMAND esl_cookbook_test)

# ---------------------------------------------------------------
# Headless physics test
#
# Verifies the Bullet bindings work end-to-end without an OpenGL
# context — drops a box on a plane, stacks two boxes, slides a
# sphere, exercises shape validation. Runs entirely on the CPU.
# ---------------------------------------------------------------
add_executable(physics_test tests/physics_test.cpp)
target_link_libraries(physics_test PRIVATE CrazyEngine)
add_test(NAME physics_test COMMAND physics_test)

# ---------------------------------------------------------------
# ESL transpiler microbenchmark (not a ctest test — manual only)
# Usage: ./esl_bench [reps] [iters]   e.g. ./esl_bench 4000 11
# ---------------------------------------------------------------
add_executable(esl_bench tests/esl_bench.cpp)
target_link_libraries(esl_bench PRIVATE CrazyEngine)
