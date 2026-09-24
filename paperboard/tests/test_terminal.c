#include "terminal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void send(PbTerminal* t,const char* s){pb_feed(t,(const uint8_t*)s,strlen(s));}
int main(void) {
    PbTerminal a,b;
    pb_init(&a);send(&a,"abc\rZ");assert(a.cells[0][0].ch=='Z'&&a.cells[0][1].ch=='b');
    send(&a,"\033[30;40HQ!");assert(a.cells[28][39].ch=='Q'&&a.cells[29][0].ch=='!');
    send(&a,"\033[2J\033[H\033[7mA\033[0mB");
    assert(a.cells[0][0].inverse&& !a.cells[0][1].inverse);
    send(&a,"\033[1;2H\033[K");assert(a.cells[0][1].ch==' '&&a.cells[0][0].ch=='A');
    send(&a,"\033]0;hidden\007X");assert(a.cells[0][1].ch=='X');
    send(&a,"\033[999999999999999999;99999H");assert(a.x==39&&a.y==29);
    // Fragmented escape sequences produce identical results.
    pb_init(&a);pb_init(&b);
    const char* text="hello\033[2;3Hworld\033[7m!\033[0m\r\nnext\033[?25l";
    send(&a,text);for(size_t i=0;i<strlen(text);i++)pb_feed(&b,(const uint8_t*)text+i,1);
    assert(!memcmp(&a,&b,sizeof(a)));
    // Arbitrary serial bytes cannot escape grid/parameter bounds (run with ASan/UBSan).
    unsigned seed=123;for(unsigned i=0;i<200000;i++) {
        seed=1664525*seed+1013904223;uint8_t ch=seed>>24;pb_feed(&a,&ch,1);
        assert(a.x<PB_COLS&&a.y<PB_ROWS&&a.nparam<8);
    }
    printf("terminal tests passed; two states: %zu bytes\n",2*sizeof(PbTerminal));
}
