#ifndef OS_API_H
#define OS_API_H

// Apps include ONLY this header. It exposes everything an app legitimately needs while hiding kernel internals (ATA, PS/2 init, memory layout, etc.)
// if it's not in this header, apps don't touch it.

// Types
#include "lib/types.h"

// String and Memory Utilities
#include "lib/string.h"
#include "lib/mem.h"
#include "lib/alloc.h"
#include "lib/math.h"

// Graphics
#include "lib/primitive_graphics.h"

// UI
#include "ui/ui.h"

// Input
#include "io/mouse.h"
#include "io/keyboard.h"

// Filesystem
#include "fs/fat16.h"

// Time
#include "lib/time.h"

// Speaker
#include "lib/speaker.h"

// Error
#include "os/error.h"

// OS services
#include "os/os.h"

// Shell CLI
#include "shell/cli.h"

#endif
