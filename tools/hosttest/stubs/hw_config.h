/* Host-test stand-in for main/hw_config.h. It must come BEFORE main/ on the include
 * path: the real header defines FS_MOUNT_POINT ("/fatfs" or "/littlefs") without an
 * #ifndef, so -D cannot override it, and the parser pastes that literal into every path
 * it opens (FS_MOUNT_POINT "/%s"). Here it is the current directory, and
 * autopid_config_test chdir()s into a fresh temp directory before each load, so the test decides exactly which auto_pid.json / car_data.json exist.
 * Nothing else from the real header is used by components/autopid/autopid_config.c. */
#ifndef GW_CONFIG_h
#define GW_CONFIG_h
#include "esp_err.h"
#define FS_MOUNT_POINT "."
#endif
