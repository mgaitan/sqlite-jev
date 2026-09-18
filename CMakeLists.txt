cmake_minimum_required(VERSION 3.15)
project(sqlite_jev VERSION 0.1.0 LANGUAGES C)

add_library(jev MODULE src/jev.c)
set_target_properties(jev PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  PREFIX ""
)
target_compile_definitions(jev PRIVATE JEV_VERSION="${PROJECT_VERSION}")

if(APPLE)
  set_target_properties(jev PROPERTIES SUFFIX ".dylib")
  target_link_options(jev PRIVATE "-undefined" "dynamic_lookup")
elseif(UNIX)
  target_link_libraries(jev PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  target_compile_options(jev PRIVATE /W4)
else()
  target_compile_options(jev PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS jev
  LIBRARY DESTINATION sqlite_jev
  RUNTIME DESTINATION sqlite_jev
)
