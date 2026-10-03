configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/konsole_dynqueue.in.json"
    "${CMAKE_CURRENT_BINARY_DIR}/konsole_dynqueue.json"
    @ONLY)

add_library(konsole_dynqueueplugin MODULE
    plugin/dynqueueplugin.cpp
    plugin/dynqueueplugin.h
    plugin/dynqueue_types.h
    plugin/dynqueuewidget.cpp
    plugin/dynqueuewidget.h
    "${CMAKE_CURRENT_BINARY_DIR}/konsole_dynqueue.json"
)

target_include_directories(konsole_dynqueueplugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin"
)

target_compile_definitions(konsole_dynqueueplugin PRIVATE
    DYNQUEUE_BUILT_KONSOLE_VERSION="${DYNQUEUE_KONSOLE_VERSION}"
)

target_link_libraries(konsole_dynqueueplugin PRIVATE
    Qt6::Core
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::XmlGui
    "${KONSOLE_APP_LIBRARY}"
    "${KONSOLE_PRIVATE_LIBRARY}"
)

set_target_properties(konsole_dynqueueplugin PROPERTIES
    OUTPUT_NAME konsole_dynqueueplugin
)

install(TARGETS konsole_dynqueueplugin
    LIBRARY DESTINATION "${DYNQUEUE_PLUGIN_INSTALL_DIR}"
)
