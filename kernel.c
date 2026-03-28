#include "lib/utils.h"

void kmain() {
	int x=20, y=0;
	bool sx=0, sy=0;
	while(1){
		clear_screen_fast(0);
		draw_rectangle(x, y, 50, 50, 0x0F);
		if(sx) x--;
		else x++;
		if(sy) y--;
		else y++;
		if(x+50>=320-10) sx=1;
		else if(x<=10) sx=0;
		if(y+50>=200-10) sy=1;
		else if(y<=10) sy=0;

		delay(4000000);
	}

    while(1);
}
