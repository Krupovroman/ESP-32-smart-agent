idf_component_register(
    SRCS "ai_client.c" "ai_prompts.c" "ai_history.c" "ai_models.c" "ai_util.c" "ai_trcheck.c"
         "tts_client.c" "tts_split.c" "tts_voices.c" "stt_client.c" "ai_cmds.c"
    INCLUDE_DIRS "."
    REQUIRES orhttp audio
    PRIV_REQUIRES settings net devconsole espressif__cjson esp_timer
)
