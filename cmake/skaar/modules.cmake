# Skaar's own game modules (C++20), replacing the stock baseq3 ones.
# Target names match the upstream ones (qagame_base...) so scripts/rebuild.sh
# and the output paths (<config>/base/*.dylib) stay the same.

if(BUILD_GAME_QVMS)
    # basegame.cmake's QVM rule builds C, and these modules are native C++20
    # libraries with no QVM entry point; say so rather than emit nothing
    message(WARNING "BUILD_GAME_QVMS: Skaar's game modules are native libraries only, no .qvm files are produced")
endif()
if(NOT BUILD_GAME_LIBRARIES)
    message(STATUS "BUILD_GAME_LIBRARIES=OFF: not building the game modules")
    return()
endif()

include(utils/set_output_dirs)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SKAAR_GAME_DIR ${CMAKE_SOURCE_DIR}/game)

# Reused Q3 C code, compiled into each module with its own define.
# Movement is the C++ port in game/shared/move_*, not bg_pmove.c / bg_slidemove.c.
set(SKAAR_MODULE_C_SOURCES
    ${SOURCE_DIR}/qcommon/q_math.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/dev_assert.c
)
set(SKAAR_BG_SOURCES
    ${SOURCE_DIR}/game/bg_misc.c
)

file(GLOB SKAAR_SHARED_SOURCES CONFIGURE_DEPENDS ${SKAAR_GAME_DIR}/shared/*.cpp)

function(skaar_module TARGET OUTPUT DEFINE)
    file(GLOB MODULE_SOURCES CONFIGURE_DEPENDS ${SKAAR_GAME_DIR}/${OUTPUT}/*.cpp)
    add_library(${TARGET} SHARED ${MODULE_SOURCES} ${SKAAR_SHARED_SOURCES} ${SKAAR_MODULE_C_SOURCES} ${ARGN})
    target_compile_definitions(${TARGET} PRIVATE ${DEFINE})
    target_include_directories(${TARGET} PRIVATE ${SOURCE_DIR} ${SKAAR_GAME_DIR} ${NLOHMANN_JSON_DIR})
    target_link_libraries(${TARGET} PRIVATE ${COMMON_LIBRARIES})
    # strict warnings on our C++ only; the reused Q3 C keeps upstream flags
    target_compile_options(${TARGET} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SKAAR_STRICT_FLAGS}>)
    if(NOT MSVC)
        # No FMA contraction anywhere in a module: move_pmove.cpp and the
        # q_math.c it calls must do the arithmetic skaar_movetest certifies as
        # byte-identical to bg_pmove.c (cmake/skaar/tests.cmake), not the one
        # clang picks by default at -O2
        target_compile_options(${TARGET} PRIVATE -ffp-contract=off)
    endif()
    set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME ${GAME_MODULE_OUTPUT_${OUTPUT}})
    set_output_dirs(${TARGET} SUBDIRECTORY ${BASEGAME})
endfunction()

set(GAME_MODULE_OUTPUT_sgame ${GAME_MODULE})
set(GAME_MODULE_OUTPUT_cgame ${CGAME_MODULE})
set(GAME_MODULE_OUTPUT_ui ${UI_MODULE})

skaar_module(qagame_${BASEGAME} sgame QAGAME ${SKAAR_BG_SOURCES})
skaar_module(cgame_${BASEGAME} cgame CGAME ${SKAAR_BG_SOURCES})
skaar_module(ui_${BASEGAME} ui UI ${SOURCE_DIR}/game/bg_misc.c)
