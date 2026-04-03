#ifndef OS_API_H
#define OS_API_H

// Apps include ONLY this header. It exposes everything an app legitimately needs while hiding kernel internals (ATA, PS/2 init, memory layout, etc.)
// if it's not in this header, apps don't touch it.

// types
#include "lib/types.h"

// string and memory utilities
#include "lib/string.h"
#include "lib/mem.h"
#include "lib/alloc.h"
#include "lib/math.h"

// graphics
#include "lib/primitive_graphics.h"

// UI
#include "ui/ui.h"

// input
#include "io/mouse.h"
#include "io/keyboard.h"

// filesystem
#include "fs/fat16.h"

// time
#include "lib/time.h"

// OS services
#include "os/os.h"

// Shell CLI (for terminal app)
#include "shell/cli.h"

#endif
