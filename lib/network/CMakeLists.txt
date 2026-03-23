if(NOT CONFIG_COMBO_BUILD)
    idf_component_register(SRCS
                           "src/mesh_net.c"
                           "src/frame_codec.c"
                           INCLUDE_DIRS "include")
endif()
