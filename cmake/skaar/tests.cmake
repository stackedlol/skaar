# Unit tests for the engine-free game logic (game/shared): `ctest` or
# <build>/tests/skaar_tests. scripts/smoke.sh runs them after the safe build.

enable_testing()

set(SKAAR_GAME_DIR ${CMAKE_SOURCE_DIR}/game)

file(GLOB SKAAR_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
add_executable(skaar_tests ${SKAAR_TEST_SOURCES}
    ${SKAAR_GAME_DIR}/shared/scenario.cpp
    ${SKAAR_GAME_DIR}/shared/runlog.cpp
    ${SKAAR_GAME_DIR}/shared/insights.cpp
    ${SKAAR_GAME_DIR}/shared/trainer_sim.cpp
    ${SKAAR_GAME_DIR}/shared/weapons.cpp
    ${SKAAR_GAME_DIR}/shared/arenalog.cpp)
target_include_directories(skaar_tests PRIVATE ${SOURCE_DIR} ${SKAAR_GAME_DIR} ${NLOHMANN_JSON_DIR})
target_compile_definitions(skaar_tests PRIVATE SKAAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
skaar_strict_target(skaar_tests)
set_target_properties(skaar_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_test(NAME skaar_tests COMMAND skaar_tests)
# a non-terminating case (a `while (!sim.Finished())` loop after a regression)
# would otherwise hang ctest for its 1500 s default
set_tests_properties(skaar_tests PROPERTIES TIMEOUT 300)

# Movement (P5, docs/plan-P5.md): the C++ pmove port against Q3's C bg_pmove.c
# (byte-identical vq3), plus the cpm ruleset's physics. Also tools/movetest/run.sh.
add_executable(skaar_movetest
    ${CMAKE_SOURCE_DIR}/tools/movetest/movetest.cpp
    ${SKAAR_GAME_DIR}/shared/move_pmove.cpp
    ${SKAAR_GAME_DIR}/shared/move_slide.cpp
    ${SKAAR_GAME_DIR}/shared/move_ruleset.cpp
    ${SOURCE_DIR}/qcommon/q_math.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/dev_assert.c
    ${SOURCE_DIR}/game/bg_misc.c
    ${SOURCE_DIR}/game/bg_pmove.c
    ${SOURCE_DIR}/game/bg_slidemove.c)
target_include_directories(skaar_movetest PRIVATE ${SOURCE_DIR} ${SKAAR_GAME_DIR} ${SKAAR_GAME_DIR}/shared ${NLOHMANN_JSON_DIR})
target_compile_definitions(skaar_movetest PRIVATE QAGAME)
# strict warnings on our C++ only; the reused Q3 C keeps upstream flags
target_compile_options(skaar_movetest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SKAAR_STRICT_FLAGS}>)
if(NOT MSVC)
    # the parity check compares C and C++ code paths, not the compiler's FMA choices
    target_compile_options(skaar_movetest PRIVATE -ffp-contract=off)
endif()
set_target_properties(skaar_movetest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_test(NAME skaar_movetest COMMAND skaar_movetest ${CMAKE_SOURCE_DIR})
