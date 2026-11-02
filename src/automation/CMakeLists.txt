# Automation bridge: a local JSON-lines command server compiled into the
# studio so external tools (e.g. the MCP server in tools/3dmm-mcp) can drive
# the movie engine. Inactive at runtime unless started with --automation.
# See doc/automation/NOTES.md.

# Engine-independent pieces, unit tested on their own
add_library(automation-core STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/image8.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/json.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/linesrv.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/pngwrite.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/review.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stage.cpp"
)
target_include_directories(automation-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if (WIN32)
    target_link_libraries(automation-core PUBLIC ws2_32)
endif()

target_sources(studio
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/actorcmd.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/blockcmd.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bridge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/catalog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/exportcmd.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/mediacmd.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/observe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/reviewcmd.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scenecmd.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/session.cpp"
)
target_compile_definitions(studio PRIVATE AUTOMATION)
target_link_libraries(studio PRIVATE automation-core)

if (BUILD_TESTS)
    add_executable(AutomationTest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/automation_test.cpp"
    )
    target_link_libraries(AutomationTest PRIVATE automation-core GTest::gtest_main)
    gtest_discover_tests(AutomationTest)
    add_dependencies(tests AutomationTest)
endif()
