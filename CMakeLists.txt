cmake_minimum_required(VERSION 3.20) 
project(EventStreamCore VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#Option 
option(ENABLE_DEBUG "Enable debug mode" ON)

#Dependencies
find_package(Boost 1.75 REQUIRED COMPONENTS filesystem system)
find_package(spdlog REQUIRED)

#Include directories
include_directories(${Boost_INCLUDE_DIRS})
include_directories(${SPDLOG_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/include)

#Source files
add_executable(EventStreamCore
    src/app/main.cpp
)
target_link_libraries(EventStreamCore PRIVATE ${Boost_LIBRARIES} spdlog::spdlog)
target_include_directories(EventStreamCore PRIVATE ${PROJECT_SOURCE_DIR}/include)
