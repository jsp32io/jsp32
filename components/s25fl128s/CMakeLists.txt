idf_component_register(
    SRCS "s25fl128s.c"
    PRIV_REQUIRES spi_flash
    LDFRAGMENTS linker.lf
    INCLUDE_DIRS ""
)

# The default driver table lives in spi_flash.  This dependency makes this
# archive available when spi_flash resolves the overridden table, including
# in the bootloader build.
idf_component_add_link_dependency(FROM spi_flash)
