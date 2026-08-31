cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/clientBrandingDefaults.cmake)

set(PROJECT ${CLIENT_TARGET_NAME})
project(${PROJECT})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY AUTOGEN_TARGETS_FOLDER "Autogen")
set_property(GLOBAL PROPERTY AUTOMOC_TARGETS_FOLDER "Autogen")
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Autogen")

set(PACKAGES
    Core Gui Network Xml
    RemoteObjects Quick Svg QuickControls2
    Core5Compat Concurrent LinguistTools
)

execute_process(
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMAND git rev-parse --short HEAD
  OUTPUT_VARIABLE GIT_COMMIT_HASH
  RESULT_VARIABLE GIT_COMMIT_HASH_RESULT
  ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT GIT_COMMIT_HASH_RESULT EQUAL 0 OR GIT_COMMIT_HASH STREQUAL "")
    set(GIT_COMMIT_HASH "unknown")
endif()

add_definitions(-DGIT_COMMIT_HASH="${GIT_COMMIT_HASH}")

add_definitions(-DPROD_AGW_PUBLIC_KEY="$ENV{PROD_AGW_PUBLIC_KEY}")
add_definitions(-DPROD_S3_ENDPOINT="$ENV{PROD_S3_ENDPOINT}")
add_definitions(-DFALLBACK_S3_ENDPOINT="$ENV{FALLBACK_S3_ENDPOINT}")

add_definitions(-DDEV_AGW_PUBLIC_KEY="$ENV{DEV_AGW_PUBLIC_KEY}")
add_definitions(-DDEV_AGW_ENDPOINT="$ENV{DEV_AGW_ENDPOINT}")
add_definitions(-DDEV_S3_ENDPOINT="$ENV{DEV_S3_ENDPOINT}")

add_definitions(-DFREE_V2_ENDPOINT="$ENV{FREE_V2_ENDPOINT}")
add_definitions(-DPREM_V1_ENDPOINT="$ENV{PREM_V1_ENDPOINT}")

if(WIN32 OR (APPLE AND NOT IOS) OR (LINUX AND NOT ANDROID))
    set(PACKAGES ${PACKAGES} Widgets)
endif()

find_package(Qt6 REQUIRED COMPONENTS ${PACKAGES})
find_package(OpenSSL REQUIRED)

if(AMNEZIA_NIXOS_BUILD)
    find_package(Qt6QSvgPlugin REQUIRED CONFIG
        PATHS "${Qt6Svg_DIR}/../Qt6Gui"
        NO_DEFAULT_PATH
    )
    find_package(Qt6QSvgIconPlugin REQUIRED CONFIG
        PATHS "${Qt6Svg_DIR}/../Qt6Gui"
        NO_DEFAULT_PATH
    )
endif()

set(LIBS ${LIBS}
    Qt6::Core Qt6::Gui
    Qt6::Network Qt6::Xml Qt6::RemoteObjects
    Qt6::Quick Qt6::Svg Qt6::QuickControls2
    Qt6::Core5Compat Qt6::Concurrent
)

if(WIN32 OR (APPLE AND NOT IOS) OR (LINUX AND NOT ANDROID))
    set(LIBS ${LIBS} Qt6::Widgets)
endif()

qt_standard_project_setup()
qt_add_executable(${PROJECT} MANUAL_FINALIZATION)
target_include_directories(${PROJECT} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_compile_definitions(${PROJECT} PRIVATE
    CLIENT_KEYCHAIN_NAME_STR=\"${CLIENT_KEYCHAIN_NAME}\"
)

if(WIN32 OR (APPLE AND NOT IOS AND NOT MACOS_NE) OR (LINUX AND NOT ANDROID))
    qt_add_repc_replicas(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/../ipc/ipc_interface.rep)
    qt_add_repc_replicas(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/../ipc/ipc_process_interface.rep)
endif()

qt6_add_resources(QRC ${QRC}
    ${CMAKE_CURRENT_LIST_DIR}/images/images.qrc
    ${CMAKE_CURRENT_LIST_DIR}/images/flagKit.qrc
    ${CMAKE_CURRENT_LIST_DIR}/client_scripts/clientScripts.qrc
    ${CLIENT_QML_QRC_FILE}
    ${CMAKE_CURRENT_LIST_DIR}/server_scripts/serverScripts.qrc
    ${CLIENT_EXTRA_QRC}
)

# -- i18n begin
if(CLIENT_TS_FILES)
    set(AMNEZIAVPN_TS_FILES ${CLIENT_TS_FILES})
else()
    set(AMNEZIAVPN_TS_FILES
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_ru_RU.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_zh_CN.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_fa_IR.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_ar_EG.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_my_MM.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_uk_UA.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_ur_PK.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_hi_IN.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_es_ES.ts
        ${CMAKE_CURRENT_LIST_DIR}/translations/${CLIENT_TS_PREFIX}_ko_KR.ts
    )
endif()

file(GLOB_RECURSE AMNEZIAVPN_QML_FILES ${CMAKE_CURRENT_LIST_DIR}/ui/qml/*.qml)

qt6_add_translations(${PROJECT}
    TS_FILES ${AMNEZIAVPN_TS_FILES}
    SOURCES ${AMNEZIAVPN_QML_FILES}
    RESOURCE_PREFIX "/translations"
)
# -- i18n end

set(IS_CI ${CI})
if(IS_CI)
    message("Detected CI env")
    find_program(CCACHE "ccache")
    if(CCACHE)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE}")
    endif()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/3rdparty.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/sources.cmake)

set(AMNEZIA_CLIENT_SHARED_SOURCES ${SOURCES})
set(AMNEZIA_CLIENT_SHARED_HEADERS ${HEADERS})
set(AMNEZIA_CLIENT_SHARED_RESOURCES ${RESOURCES})
set(AMNEZIA_CLIENT_SHARED_QRC ${QRC})
set(AMNEZIA_CLIENT_SHARED_I18N_QRC ${I18NQRC})

set(_client_include_dirs
    ${CMAKE_CURRENT_LIST_DIR}/../ipc
    ${CMAKE_CURRENT_LIST_DIR}/../common/logger
    ${CMAKE_CURRENT_LIST_DIR}/../common/crypto
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
if(CLIENT_BRANDING_INCLUDE_DIR)
    list(APPEND _client_include_dirs ${CLIENT_BRANDING_INCLUDE_DIR})
endif()
list(APPEND _client_include_dirs ${CMAKE_CURRENT_LIST_DIR})
include_directories(${_client_include_dirs})

if(MACOS_NE)
    message("MACOS_NE is ON")
    add_definitions(-DQ_OS_MAC)
    add_definitions(-DMACOS_NE)
    message("Add macros for MacOS Network Extension")
else()
    message("MACOS_NE is OFF")
endif()

include_directories(mozilla)
include_directories(mozilla/shared)
include_directories(mozilla/models)

configure_file(${CMAKE_CURRENT_LIST_DIR}/../version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/core/utils)
configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/core/utils/appUiConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/core/utils/appUiConfig.h
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/core/utils/swiftBridgeConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/core/utils/swiftBridgeConfig.h
    @ONLY
)
# Swift can't consume a `-DGROUP_ID="value"` compile define (Swift -D flags are
# presence-only), unlike the Objective-C++ sources that read GROUP_ID directly.
# Generate a Swift constant instead so Log.swift can use the branded app group.
set(CLIENT_GROUP_IDENTIFIER_SWIFT_FILE ${CMAKE_CURRENT_BINARY_DIR}/platforms/ios/GroupIdentifier.swift)
configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/platforms/ios/GroupIdentifier.swift.in
    ${CLIENT_GROUP_IDENTIFIER_SWIFT_FILE}
    @ONLY
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT} PRIVATE "MZ_DEBUG")
endif()

if(WIN32)
    configure_file(
        ${CMAKE_CURRENT_LIST_DIR}/platforms/windows/amneziavpn.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/amneziavpn.rc
    )

    set(LIBS ${LIBS}
        user32
        rasapi32
        shlwapi
        iphlpapi
        ws2_32
        gdi32
    )

endif()

if(APPLE)
    cmake_policy(SET CMP0099 NEW)
    cmake_policy(SET CMP0114 NEW)


    set(CMAKE_XCODE_GENERATE_SCHEME FALSE)
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ${BUILD_VPN_DEVELOPMENT_TEAM})
    set(CMAKE_XCODE_ATTRIBUTE_GROUP_ID_IOS ${BUILD_IOS_GROUP_IDENTIFIER})
    
    if (BUILD_VPN_KEYCHAIN)
        set(CMAKE_XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--keychain ${BUILD_VPN_KEYCHAIN}")
    endif()
endif()

if(LINUX AND NOT ANDROID)
    set(LIBS ${LIBS} -static-libstdc++ -static-libgcc -ldl)
    link_directories(${CMAKE_CURRENT_LIST_DIR}/platforms/linux)
    set(LIBS ${LIBS} -Wl,--no-as-needed $<TARGET_LINKER_FILE:Qt6::RemoteObjects> -Wl,--as-needed)
endif()

if(WIN32 OR (APPLE AND NOT IOS AND NOT MACOS_NE) OR (LINUX AND NOT ANDROID))
    add_compile_definitions(AMNEZIA_DESKTOP)
endif()

if(ANDROID)
    include(cmake/android.cmake)
endif()

if(IOS)
    include(cmake/ios.cmake)
    include(cmake/ios-arch-fixup.cmake)
elseif(APPLE AND MACOS_NE)
    include(cmake/macos_ne.cmake)
elseif(APPLE)
    include(cmake/osxtools.cmake)
    include(cmake/macos.cmake)
endif()

list(APPEND SOURCES ${CMAKE_CURRENT_LIST_DIR}/main.cpp)

target_link_libraries(${PROJECT} PRIVATE ${LIBS})
target_compile_definitions(${PROJECT} PRIVATE "MZ_$<UPPER_CASE:${MZ_PLATFORM_NAME}>")
if(WIN32)
    target_link_options(${PROJECT} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

target_sources(${PROJECT} PRIVATE ${SOURCES} ${HEADERS} ${RESOURCES} ${QRC} ${I18NQRC})

# Finalize the executable so Qt can gather/deploy QML modules and plugins correctly (Android needs this).
if(COMMAND qt_import_qml_plugins)
    qt_import_qml_plugins(${PROJECT})
endif()
if(COMMAND qt_finalize_executable)
    qt_finalize_executable(${PROJECT})
else()
    qt_finalize_target(${PROJECT})
endif()

install(TARGETS ${PROJECT}
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RUNTIME_DEPENDENCY_SET client_deps
    COMPONENT AmneziaVPNGui
)

if(APPLE)
    set(RUNTIME_DEPS_DIR ${CMAKE_INSTALL_BINDIR}/AmneziaVPN.app/Contents/Frameworks)
else()
    set(RUNTIME_DEPS_DIR ${CMAKE_INSTALL_BINDIR})
endif()

install(RUNTIME_DEPENDENCY_SET client_deps
    PRE_EXCLUDE_REGEXES
        [[api-ms-win-.*]]
        [[ext-ms-.*]]
        [[kernel32\.dll]]
        [[hvsifiletrust\.dll]]
        [[libc\.so\..*]] [[libgcc_s\.so\..*]] [[libm\.so\..*]] [[libstdc\+\+\.so\..*]]
        [[.*\.framework]]
        [[^(lib)?[Qq]t.*]]
    POST_EXCLUDE_REGEXES
        [[^.*[\\/]system32[\\/].*\.dll$]]
        [[^/lib.*]]
        [[^/usr/lib.*]]
    DIRECTORIES ${CONAN_RUNTIME_LIB_DIRS}
    COMPONENT AmneziaVPNGui
    DESTINATION "${RUNTIME_DEPS_DIR}"
)

set(deploy_tool_options "")
if(WIN32)
    set(deploy_tool_options "--force-openssl --force")
endif()

if(AMNEZIA_NIXOS_BUILD)
    qt_generate_deploy_script(
        TARGET ${PROJECT}
        OUTPUT_SCRIPT QT_DEPLOY_SCRIPT
        CONTENT "
qt6_deploy_qml_imports(TARGET ${PROJECT} PLUGINS_FOUND plugins_found)
qt6_deploy_runtime_dependencies(
    EXECUTABLE \"$<TARGET_FILE:${PROJECT}>\"
    ADDITIONAL_MODULES \\\${plugins_found}
    GENERATE_QT_CONF
    EXCLUDE_PLUGINS qgtk3
    EXCLUDE_PLUGIN_TYPES sqldrivers
)"
    )
else()
    qt_generate_deploy_qml_app_script(
        TARGET ${PROJECT}
        OUTPUT_SCRIPT QT_DEPLOY_SCRIPT
        NO_UNSUPPORTED_PLATFORM_ERROR
        DEPLOY_TOOL_OPTIONS ${deploy_tool_options}
    )
endif()
install(SCRIPT ${QT_DEPLOY_SCRIPT}
    COMPONENT AmneziaVPNGui
)
if(AMNEZIA_NIXOS_BUILD)
    install(FILES "$<TARGET_FILE:Qt6::QSvgPlugin>"
        DESTINATION "${QT6_INSTALL_PLUGINS}/imageformats"
        COMPONENT AmneziaVPNGui
    )
    install(FILES "$<TARGET_FILE:Qt6::QSvgIconPlugin>"
        DESTINATION "${QT6_INSTALL_PLUGINS}/iconengines"
        COMPONENT AmneziaVPNGui
    )
endif()

if(WIN32 OR (APPLE AND NOT IOS AND NOT MACOS_NE) OR (LINUX AND NOT ANDROID))
    set(AMNEZIA_CLI_TARGET AmneziaVPN-cli)
    qt_add_executable(${AMNEZIA_CLI_TARGET} MANUAL_FINALIZATION)
    set_target_properties(${AMNEZIA_CLI_TARGET} PROPERTIES OUTPUT_NAME amnezia)

    target_include_directories(${AMNEZIA_CLI_TARGET} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    )

    set(AMNEZIA_CLI_REPC_DIR ${CMAKE_CURRENT_BINARY_DIR}/cli_repc)
    file(MAKE_DIRECTORY ${AMNEZIA_CLI_REPC_DIR})
    target_include_directories(${AMNEZIA_CLI_TARGET} BEFORE PRIVATE ${AMNEZIA_CLI_REPC_DIR})

    set(AMNEZIA_CLI_REPLICA_HEADERS)
    foreach(rep_file
            ${CMAKE_CURRENT_LIST_DIR}/../ipc/ipc_interface.rep
            ${CMAKE_CURRENT_LIST_DIR}/../ipc/ipc_process_interface.rep)
        get_filename_component(rep_name ${rep_file} NAME_WE)
        set(replica_header ${AMNEZIA_CLI_REPC_DIR}/rep_${rep_name}_replica.h)
        add_custom_command(
            OUTPUT ${replica_header}
            COMMAND Qt6::repc -o replica ${rep_file} ${replica_header}
            MAIN_DEPENDENCY ${rep_file}
            DEPENDS Qt6::repc
            VERBATIM
        )
        list(APPEND AMNEZIA_CLI_REPLICA_HEADERS ${replica_header})
    endforeach()
    qt6_wrap_cpp(AMNEZIA_CLI_REPLICA_MOCS ${AMNEZIA_CLI_REPLICA_HEADERS} TARGET ${AMNEZIA_CLI_TARGET})
    set_source_files_properties(
        ${AMNEZIA_CLI_REPLICA_HEADERS}
        ${AMNEZIA_CLI_REPLICA_MOCS}
        PROPERTIES GENERATED TRUE SKIP_AUTOGEN TRUE
    )

    target_link_libraries(${AMNEZIA_CLI_TARGET} PRIVATE ${LIBS})
    target_compile_definitions(${AMNEZIA_CLI_TARGET} PRIVATE
        "MZ_$<UPPER_CASE:${MZ_PLATFORM_NAME}>"
        AMNEZIA_CLI
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(${AMNEZIA_CLI_TARGET} PRIVATE "MZ_DEBUG")
    endif()

    if(WIN32)
        target_link_options(${AMNEZIA_CLI_TARGET} PRIVATE /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup)
    endif()

    target_sources(${AMNEZIA_CLI_TARGET} PRIVATE
        ${AMNEZIA_CLIENT_SHARED_SOURCES}
        ${AMNEZIA_CLIENT_SHARED_HEADERS}
        ${AMNEZIA_CLIENT_SHARED_RESOURCES}
        ${AMNEZIA_CLIENT_SHARED_QRC}
        ${AMNEZIA_CLIENT_SHARED_I18N_QRC}
        ${AMNEZIA_CLI_REPLICA_HEADERS}
        ${AMNEZIA_CLI_REPLICA_MOCS}
        ${CMAKE_CURRENT_LIST_DIR}/cli/main.cpp
        ${CMAKE_CURRENT_LIST_DIR}/cli/subscriptionCodec.cpp
        ${CMAKE_CURRENT_LIST_DIR}/cli/subscriptionCodec.h
        ${CMAKE_CURRENT_LIST_DIR}/cli/terminal.cpp
        ${CMAKE_CURRENT_LIST_DIR}/cli/terminal.h
    )

    if(COMMAND qt_import_qml_plugins)
        qt_import_qml_plugins(${AMNEZIA_CLI_TARGET})
    endif()
    if(COMMAND qt_finalize_executable)
        qt_finalize_executable(${AMNEZIA_CLI_TARGET})
    else()
        qt_finalize_target(${AMNEZIA_CLI_TARGET})
    endif()

    add_custom_command(TARGET ${AMNEZIA_CLI_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${AMNEZIA_CLI_TARGET}>"
            "$<TARGET_FILE_DIR:${AMNEZIA_CLI_TARGET}>/vpn$<TARGET_FILE_SUFFIX:${AMNEZIA_CLI_TARGET}>"
    )

    install(TARGETS ${AMNEZIA_CLI_TARGET}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RUNTIME_DEPENDENCY_SET cli_deps
        COMPONENT AmneziaVPNCore
    )
    install(PROGRAMS
        "$<TARGET_FILE_DIR:${AMNEZIA_CLI_TARGET}>/vpn$<TARGET_FILE_SUFFIX:${AMNEZIA_CLI_TARGET}>"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT AmneziaVPNCore
    )

    install(RUNTIME_DEPENDENCY_SET cli_deps
        PRE_EXCLUDE_REGEXES
            [[api-ms-win-.*]]
            [[ext-ms-.*]]
            [[kernel32\.dll]]
            [[hvsifiletrust\.dll]]
            [[libc\.so\..*]] [[libgcc_s\.so\..*]] [[libm\.so\..*]] [[libstdc\+\+\.so\..*]]
            [[.*\.framework]]
            [[^[Qq]t.*]]
        POST_EXCLUDE_REGEXES
            [[^.*[\\/]system32[\\/].*\.dll$]]
            [[^/lib.*]]
            [[^/usr/lib.*]]
        DIRECTORIES ${CONAN_RUNTIME_LIB_DIRS}
        COMPONENT AmneziaVPNCore
        DESTINATION "${RUNTIME_DEPS_DIR}"
    )

    if(AMNEZIA_NIXOS_BUILD)
        qt_generate_deploy_script(
            TARGET ${AMNEZIA_CLI_TARGET}
            OUTPUT_SCRIPT AMNEZIA_CLI_DEPLOY_SCRIPT
            CONTENT "
qt6_deploy_runtime_dependencies(
    EXECUTABLE \"$<TARGET_FILE:${AMNEZIA_CLI_TARGET}>\"
    GENERATE_QT_CONF
    EXCLUDE_PLUGINS qgtk3
    EXCLUDE_PLUGIN_TYPES sqldrivers
)"
        )
    else()
        qt_generate_deploy_app_script(
            TARGET ${AMNEZIA_CLI_TARGET}
            OUTPUT_SCRIPT AMNEZIA_CLI_DEPLOY_SCRIPT
            NO_UNSUPPORTED_PLATFORM_ERROR
            DEPLOY_TOOL_OPTIONS ${deploy_tool_options}
        )
    endif()
    install(SCRIPT ${AMNEZIA_CLI_DEPLOY_SCRIPT}
        COMPONENT AmneziaVPNCore
    )
endif()

if (APPLE AND NOT IOS AND NOT MACOS_NE)
    list(APPEND OVPN_SCRIPTS "${CMAKE_SOURCE_DIR}/deploy/data/macos/update-resolv-conf.sh")
    set_target_properties(${PROJECT} PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    )
endif()
if (LINUX AND NOT ANDROID)
    list(APPEND OVPN_SCRIPTS "${CMAKE_SOURCE_DIR}/deploy/data/linux/update-resolv-conf.sh")
endif()

if(OVPN_SCRIPTS)
    add_custom_command(TARGET ${PROJECT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${OVPN_SCRIPTS}
        "$<TARGET_FILE_DIR:${PROJECT}>"
    )

    install(FILES ${OVPN_SCRIPTS}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT AmneziaVPNCore
        PERMISSIONS
            OWNER_READ OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
    )
endif()
