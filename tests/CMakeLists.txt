# Unit tests for the parts of the effect that do not need a compositor:
# the sampling ring buffer and the cursor geometry.
add_executable(trailsample_test
    trailsample_test.cpp
    ${CMAKE_SOURCE_DIR}/src/trailsample.cpp
)

target_include_directories(trailsample_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(trailsample_test PRIVATE Qt6::Core)

add_test(NAME trailsample COMMAND trailsample_test)

# Verifies that KWin's effect loader would find the built plugin.
add_executable(plugin_metadata_test plugin_metadata_test.cpp)
target_link_libraries(plugin_metadata_test PRIVATE Qt6::Core KF6::CoreAddons)

add_test(NAME plugin_metadata
    COMMAND plugin_metadata_test "${CMAKE_BINARY_DIR}/plugins"
)
