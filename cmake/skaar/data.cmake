# Expose data/<BASEGAME> to the engine inside the build tree (see deploy_data.cmake)

if(CMAKE_CONFIGURATION_TYPES)
    set(SKAAR_DATA_OUT_DIR ${CMAKE_BINARY_DIR}/$<CONFIG>/${BASEGAME})
else()
    set(SKAAR_DATA_OUT_DIR ${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}/${BASEGAME})
endif()

add_custom_target(skaar_data ALL
    COMMAND ${CMAKE_COMMAND}
        -DDATA_DIR=${CMAKE_SOURCE_DIR}/data/${BASEGAME}
        -DOUT_DIR=${SKAAR_DATA_OUT_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/skaar/deploy_data.cmake
    COMMENT "Linking data/${BASEGAME} into the build tree"
    VERBATIM)

# `cmake --install` / cpack only install targets (cmake/installer.cmake), so an
# installed game had no scenarios, weapons, rulesets or maps and every runtime
# load failed. Install them next to the modules, which go to ./<BASEGAME>.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/${BASEGAME}/ DESTINATION ./${BASEGAME})
install(DIRECTORY ${SKAAR_DATA_OUT_DIR}/generated.pk3dir/ DESTINATION ./${BASEGAME}/generated.pk3dir)
