cmake_minimum_required(VERSION 3.10)
project(stock-market-server)

set(CMAKE_CXX_STANDARD 17)

# Find required packages
find_package(Boost REQUIRED COMPONENTS system)
find_package(CURL REQUIRED)

add_executable(stock-market-server main.cpp)

target_link_libraries(stock-market-server Boost::system CURL::libcurl)
