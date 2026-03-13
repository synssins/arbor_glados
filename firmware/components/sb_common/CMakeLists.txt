# sb_common — shared interface headers for cross-component use
# Headers: plugin_manager.h, event_bus.h, app_config.h
# Implementations stay in main component; this just provides the includes.

idf_component_register(
    SRCS "sb_common.c"
    INCLUDE_DIRS "include"
    REQUIRES json
)
