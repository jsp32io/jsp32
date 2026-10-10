# Local LittleFS component

This is the `joltwallet/esp_littlefs` v1.22.3 component (commit
`bde79e13be971a1f06d5e5f55e6f3e8568fa32b5`) with its LittleFS submodule
(commit `6cb4e86540eca0d9ba62500a298385c9d863c8be`) vendored locally.
The upstream component and the LittleFS core licenses are in `LICENSE` and
`src/littlefs/LICENSE.md`.

The S25FL128S main array cannot erase 4 KiB sectors. This local copy adds a
configurable LittleFS block size and uses it for both partition operations
and the generated image. `sdkconfig.defaults` selects 64 KiB blocks, matching
the flash's physical erase blocks and the aligned `storage` partition.
