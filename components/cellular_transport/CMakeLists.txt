# VERIFY: "walter-modem" is a guess at the component name registered by
# the dptechnics/walter-modem managed dependency (declared in
# main/idf_component.yml) - not confirmed, since that package isn't
# fetched into this checkout. Check the actual name idf.py reports
# under managed_components/ after a `idf.py reconfigure` and fix this
# REQUIRES entry if it differs.
idf_component_register(
    SRCS "cellular_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES config_store freertos walter-modem
)
