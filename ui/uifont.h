#ifndef UIFONT_H
#define UIFONT_H

/* The fonts the desktop uses (lib/graphics.h sizes). Settings → Display
 * "Classic fonts" turns both back into the old 4x6 font. */
#include "config/runtime_config.h"
#include "fonts/fonts.h"

#define UI_FONT_BOLD (g_cfg.classic_fonts ? FONT_MONO : FONT_SYSTEM)
#define UI_FONT_SMALL (g_cfg.classic_fonts ? FONT_MONO : FONT_SMALL)

#endif
