# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "E:/ESPIDF/frameworks/esp-idf-v6.0.3/.espressif/v6.0.3/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "E:/ESPIDF/frameworks/esp-idf-v6.0.3/.espressif/v6.0.3/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "E:/vscodeespidf/projectworkspace/components-new/i2s_spk/i2s_spk/test_app/build_iram_off/bootloader"
  "E:/vscodeespidf/projectworkspace/components-new/i2s_spk/i2s_spk/test_app/build_iram_off/bootloader-prefix"
  "E:/vscodeespidf/projectworkspace/components-new/i2s_spk/i2s_spk/test_app/build_iram_off/bootloader-prefix/tmp"
  "E:/vscodeespidf/projectworkspace/components-new/i2s_spk/i2s_spk/test_app/build_iram_off/bootloader-prefix/src/bootloader-stamp"
  "E:/vscodeespidf/projectworkspace/components-new/i2s_spk/i2s_spk/test_app/build_iram_off/bootloader-prefix/src"
  "E:/vscodeespidf/projectworkspace/components-new/i2s_spk/i2s_spk/test_app/build_iram_off/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "E:/vscodeespidf/projectworkspace/components-new/i2s_spk/i2s_spk/test_app/build_iram_off/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "E:/vscodeespidf/projectworkspace/components-new/i2s_spk/i2s_spk/test_app/build_iram_off/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
