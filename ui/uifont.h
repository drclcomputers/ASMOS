#ifndef UIFONT_H
#define UIFONT_H

#include "config/runtime_config.h"
#include "fonts/fonts.h"

#define UI_FONT_BOLD (g_cfg.classic_fonts ? FONT_MONO : FONT_SYSTEM)
#define UI_FONT_SMALL (g_cfg.classic_fonts ? FONT_MONO : FONT_SMALL)
#define UI_FONT_TERM (g_cfg.classic_fonts ? FONT_MONO : FONT_TERM)
#define UI_TERM_CW (g_cfg.classic_fonts ? 5 : FONT_TERM_W)

#endif
