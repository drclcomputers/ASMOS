#include "lib/math.h"

int abs(int x){
	if(x<0) return -x;
	return x;
}

int max(int a, int b){
	if(a>b) return a;
	return b;
}

int min(int a, int b){
	if(a<b) return a;
	return b;
}
