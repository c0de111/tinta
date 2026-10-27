# Find all source files in a single current directory
# Save the name to DIR_ePaper_SRCS
aux_source_directory(. DIR_ePaper_SRCS)

include_directories(../Config)

# Generate the link library
add_library(ePaper ${DIR_ePaper_SRCS})
target_link_libraries(ePaper PUBLIC Config)
# debug.h for refresh timing logs; symbols resolve from the firmware's debug.c
target_include_directories(ePaper PRIVATE ${CMAKE_SOURCE_DIR})