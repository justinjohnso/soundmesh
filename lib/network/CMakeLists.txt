if(NOT CONFIG_SRC_BUILD)
    idf_component_register(SRCS
                           "src/mesh_net.c"
                           "src/audio_transport.c"
                           "src/frame_codec.c"
                           "src/uplink_control.c"
                           "src/mixer_control.c"
                           "src/mesh/mesh_state.c"
                           "src/mesh/mesh_identity.c"
                           "src/mesh/mesh_dedupe.c"
                           "src/mesh/mesh_uplink.c"
                           "src/mesh/mesh_mixer.c"
                           "src/mesh/mesh_events.c"
                           "src/mesh/mesh_rx.c"
                           "src/mesh/mesh_ping.c"
                           "src/mesh/mesh_tx.c"
                           "src/mesh/mesh_queries.c"
                           "src/mesh/mesh_heartbeat.c"
                           "src/mesh/mesh_init.c"
                           INCLUDE_DIRS "include"
                           PRIV_INCLUDE_DIRS "src")
endif()
