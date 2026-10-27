# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# libsodium: Ed25519-verify + SHA-256 OTA-пакетов (ota_sec.c);
# mbedtls: AES-128-CTR потоковой расшифровки
idf_component_register(SRCS ${app_sources} REQUIRES libsodium mbedtls)
