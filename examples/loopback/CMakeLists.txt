cmake_minimum_required(VERSION 3.16)

# Vendors i2s_spk locally (this example lives inside the i2s_spk
# repository) — i2s_mic is instead resolved via the ESP Component Manager
# from main/idf_component.yml.
set(EXTRA_COMPONENT_DIRS
    ../../
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(i2s_spk_loopback_example)
