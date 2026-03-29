#include "lib/utils.h"
#include "ui/ui.h"

void kmain() {
	draw_window(20, 20, 100, 70, "Hello World");
	blit();

    while(1);
}
