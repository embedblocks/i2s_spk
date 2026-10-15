cmake_minimum_required(VERSION 3.16)

# Builds against the local i2s_spk checkout (this test app lives inside the
# i2s_spk repository), the same way the examples do.
set(EXTRA_COMPONENT_DIRS
    ../
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(i2s_spk_test_app)
