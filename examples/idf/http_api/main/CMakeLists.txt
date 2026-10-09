idf_component_register(SRCS "main.cpp")
# Build with -DFOOTPRINT_BASELINE=1 to measure the same app without TesserAPI.
if(FOOTPRINT_BASELINE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FOOTPRINT_BASELINE=1)
endif()
