cmake_minimum_required(VERSION 3.14)

project(sdbuscpp_deprecated_collector CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)
find_package(CURL REQUIRED)

add_executable(sdbus-deprecated-collector collector.cpp)
target_compile_features(sdbus-deprecated-collector PRIVATE cxx_std_17)
target_link_libraries(sdbus-deprecated-collector PRIVATE PkgConfig::SDBUSCPP)
target_link_libraries(sdbus-deprecated-collector PRIVATE CURL::libcurl)
