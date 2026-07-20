find_program(CONAN_COMMAND "conan" REQUIRED
    HINTS
        "${CMAKE_SOURCE_DIR}/.venv/bin"
        "/opt/homebrew/bin"
)

file(GLOB_RECURSE LOCAL_RECIPES "${CMAKE_SOURCE_DIR}/recipes/*/conanfile.py")
list(REMOVE_ITEM LOCAL_RECIPES "${CMAKE_SOURCE_DIR}/recipes/go/conanfile.py")
foreach(RECIPE ${LOCAL_RECIPES})
    get_filename_component(RECIPE_DIR ${RECIPE} DIRECTORY)
    execute_process(
        COMMAND ${CONAN_COMMAND} export ${RECIPE_DIR}
    )
endforeach()

# FIXME(ygurov): export all versions declared on recipies_bootstrap call
execute_process(
    COMMAND ${CONAN_COMMAND} export "${CMAKE_SOURCE_DIR}/recipes/go" --version 1.26.0
)
execute_process(
    COMMAND ${CONAN_COMMAND} export "${CMAKE_SOURCE_DIR}/recipes/go" --version 1.23.12
)

set(
    AMNEZIA_CONAN_REMOTE_URL
    "https://artifactory.amnezia.org/artifactory/api/conan/client-prebuilts"
    CACHE STRING
    "Amnezia Conan binary remote URL"
)
option(AMNEZIA_CONAN_USE_REMOTE "Use the Amnezia Conan binary remote" ON)

execute_process(
    COMMAND ${CONAN_COMMAND} remote add amnezia "${AMNEZIA_CONAN_REMOTE_URL}" --force
    RESULT_VARIABLE AMNEZIA_CONAN_REMOTE_RESULT
)
if(NOT AMNEZIA_CONAN_REMOTE_RESULT EQUAL 0)
    message(FATAL_ERROR "Unable to configure the Amnezia Conan remote")
endif()

if(NOT AMNEZIA_CONAN_USE_REMOTE)
    execute_process(
        COMMAND ${CONAN_COMMAND} remote disable amnezia
        RESULT_VARIABLE AMNEZIA_CONAN_DISABLE_REMOTE_RESULT
    )
    if(NOT AMNEZIA_CONAN_DISABLE_REMOTE_RESULT EQUAL 0)
        message(FATAL_ERROR "Unable to disable the Amnezia Conan remote")
    endif()
    message(STATUS "Amnezia Conan remote disabled; missing packages will build from source")
endif()
