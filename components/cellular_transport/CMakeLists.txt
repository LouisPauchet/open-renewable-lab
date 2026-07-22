# dptechnics/walter-modem only supports the esp32s3 target (see
# main/idf_component.yml) - only pull it in when actually building for
# that target, so the ESP32 DevKit V1 test-rig target (plain esp32)
# doesn't hit a dependency-resolution error for a component it never
# uses. cellular_transport.cpp/cellular_transport_cpp.h mirror this
# with matching `#if CONFIG_IDF_TARGET_ESP32S3` guards.
#
# VERIFY: "walter-modem" is a guess at the component name registered by
# the dptechnics/walter-modem managed dependency - not confirmed until
# an esp32s3 build actually fetches it into managed_components/ and we
# can check. Fix this REQUIRES entry if it differs.
set(walter_modem_req "")
if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND walter_modem_req walter-modem)
endif()

idf_component_register(
    SRCS "cellular_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES config_store freertos ${walter_modem_req}
)
