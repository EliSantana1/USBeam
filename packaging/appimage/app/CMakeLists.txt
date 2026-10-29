# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the USBeam window/tray app for the AppImage.
cmake_minimum_required(VERSION 3.16)
project(usbeam_app CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network WebEngineWidgets)
add_executable(usbeam-app ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/app/usbeam_app.cpp)
target_link_libraries(usbeam-app PRIVATE Qt6::Widgets Qt6::Network Qt6::WebEngineWidgets)
install(TARGETS usbeam-app DESTINATION bin)
