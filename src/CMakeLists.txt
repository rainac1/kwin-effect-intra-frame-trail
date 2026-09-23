# The effect plugin itself. kcoreaddons_add_plugin() builds a Qt plugin that
# embeds metadata.json as its KPluginMetaData and installs it into the KWin
# effect plugin namespace.
kcoreaddons_add_plugin(trail
    SOURCES
        main.cpp
        traileffect.cpp
        traileffect.h
        trailsample.cpp
        trailsample.h
        metadata.json
    INSTALL_NAMESPACE "kwin/effects/plugins"
)

# KDE plugins are conventionally installed without the "lib" prefix, so that the
# file name matches the plugin id.
set_target_properties(trail PROPERTIES PREFIX "")

target_link_libraries(trail PRIVATE
    KWin::kwin
    Qt6::Core
    Qt6::Gui
    KF6::ConfigCore
    KF6::CoreAddons
)
