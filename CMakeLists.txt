cmake_minimum_required(VERSION 3.13)
project(sdrpp-spots)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

include(FindPkgConfig)
pkg_check_modules(CURL libcurl REQUIRED)
include_directories(SYSTEM ${CURL_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${CURL_LIBRARIES})

#add_library(${PROJECT_NAME} SHARED ${SRC})
#target_link_libraries(${PROJECT_NAME} PRIVATE sdrpp_core)
#set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
#target_include_directories(${PROJECT_NAME} PRIVATE "src/")
#set(SDRPP_MODULE_COMPILER_FLAGS @SDRPP_MODULE_COMPILER_FLAGS@)
#install(TARGETS ${PROJECT_NAME} DESTINATION lib/sdrpp/plugins)
