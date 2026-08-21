# The otool_llm_sdk component lives two levels above this test app.
set(EXTRA_COMPONENT_DIRS ../../)

idf_component_register(
    SRCS "main/main.c"
    INCLUDE_DIRS "."
    REQUIRES otool_llm_sdk
)
