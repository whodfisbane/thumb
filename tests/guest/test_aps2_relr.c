// ldflags: -Wl,--pack-dyn-relocs=android+relr -Wl,--use-android-relr-tags
/* Both packed formats, RELR under Android's pre-standard DT_ANDROID_RELR tags. */
#include "relocs.h"

TEST_MAIN(check_relocs())
