#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEST=${1:-/tmp/ss4s-opus71-tests}
CC=${CC:-cc}
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "$ROOT/tests/test_max_res.c" -I"$ROOT/include" -ldl -o "$DEST-max-res"
"$DEST-max-res"
# Intentionally no device access: libopus and GStreamer CAPS objects only.
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "$ROOT/tests/test_opus71.c" \
    "$ROOT/modules/webos/ndl/webos5/opus_config.c" \
    "$ROOT/modules/webos/ndl/webos5/opus71_route.c" \
    -I"$ROOT/modules/webos/ndl/webos5" \
    $(pkg-config --cflags --libs gstreamer-1.0 opus) -lpthread -ldl -lm -o "$DEST"
GST_REGISTRY_FORK=no "$DEST"
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "$ROOT/tests/test_opus71_hooks.c" \
    "$ROOT/modules/webos/ndl/webos5/opus_config.c" \
    "$ROOT/modules/webos/ndl/webos5/opus71_route.c" \
    -I"$ROOT/modules/webos/ndl/webos5" \
    $(pkg-config --cflags --libs gstreamer-1.0) -lpthread -ldl -o "$DEST-hooks"
GST_REGISTRY_FORK=no "$DEST-hooks"
