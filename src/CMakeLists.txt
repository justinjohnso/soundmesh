# Conditional source selection based on CONFIG_TX_BUILD flag
if(CONFIG_TX_BUILD)
    set(app_sources "tx/main.c")
    message(STATUS "Building TX firmware")
else()
    set(app_sources "rx/main.c")
    message(STATUS "Building RX firmware")
endif()

idf_component_register(SRCS ${app_sources})
