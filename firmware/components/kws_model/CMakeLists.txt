idf_component_register(
    SRCS "kws_model_format.c" "kws_model_store.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition
    PRIV_REQUIRES nvs_flash mbedtls log heap
    EMBED_FILES "models/kws_factory.kwsm"
    EMBED_TXTFILES "keys/model_signing_pub.pem")
