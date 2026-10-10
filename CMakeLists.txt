cmake_minimum_required(VERSION 3.20)
project(Retina VERSION 0.2.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(Boost_NO_BOOST_CMAKE ON)
find_path(BOOST_INCLUDE_DIR boost/process.hpp REQUIRED)
find_path(BOOST_JSON_INCLUDE_DIR boost/json.hpp REQUIRED)

add_library(retina_core
    src/agent.cpp
    src/client.cpp
    src/json.cpp
)
target_include_directories(retina_core PUBLIC include ${BOOST_INCLUDE_DIR} ${BOOST_JSON_INCLUDE_DIR})
target_compile_features(retina_core PUBLIC cxx_std_20)
target_compile_definitions(retina_core PUBLIC BOOST_JSON_HEADER_ONLY)

if(WIN32)
    # Boost.JSON is compiled into src/json.cpp via <boost/json/src.hpp>.
    # Disable MSVC's auto-linking to prebuilt Boost libraries.
    target_compile_definitions(retina_core PUBLIC
        BOOST_JSON_NO_LIB
        BOOST_CONTAINER_NO_LIB
        _WIN32_WINNT=0x0A00
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(retina_core PUBLIC pthread)
endif()

add_executable(retina-agent src/main.cpp)
target_link_libraries(retina-agent PRIVATE retina_core)

add_executable(retina-tests tests/retina_tests.cpp)
target_link_libraries(retina-tests PRIVATE retina_core)

enable_testing()
add_test(NAME retina_tests COMMAND retina-tests $<TARGET_FILE:retina-agent>)

install(TARGETS retina-agent retina-tests retina_core
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
