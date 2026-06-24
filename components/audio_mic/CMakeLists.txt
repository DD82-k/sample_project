idf_component_register(SRCS "audio_mic.c" "vad.c" "alarm_detect.c" "speech_prep.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver)
