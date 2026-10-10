if(DEFINED CONFIG_LITTLEFS_BLOCK_SIZE AND NOT CONFIG_LITTLEFS_BLOCK_SIZE EQUAL 65536)
    message(FATAL_ERROR "S25FL128S storage requires 64 KiB LittleFS erase blocks")
endif()
if(CONFIG_SPI_FLASH_BYPASS_BLOCK_ERASE)
    message(FATAL_ERROR "S25FL128S storage requires SPI flash block erase")
endif()

idf_component_register(
    SRCS "app_filesystem.c"
    INCLUDE_DIRS "include"
    REQUIRES littlefs mbedtls
)

littlefs_create_partition_image(storage ${PROJECT_DIR}/rootfs FLASH_IN_PROJECT)
