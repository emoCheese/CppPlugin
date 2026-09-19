add_library(nlohmann_json
    INTERFACE
        json.hpp
)

add_library(plugin::json ALIAS nlohmann_json)

target_include_directories(nlohmann_json
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)