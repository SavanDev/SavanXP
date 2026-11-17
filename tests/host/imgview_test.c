/* Viewport outcomes, independent of SUN handles and the GUI host. */
#include <stdio.h>
#include <string.h>
#include "view.h"
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n",__LINE__,#c); ++failures; } } while (0)
int main(void)
{
    struct iv_view v={640,400,0,0,1000,1,0,0};
    iv_view_resize(&v,1000,600);
    CHECK(v.zoom==1000 && iv_view_width(&v)==640 && iv_view_height(&v)==400);
    iv_view_resize(&v,320,200);
    CHECK(v.zoom==500 && iv_view_width(&v)==320 && iv_view_height(&v)==200);
    iv_view_step(&v,1);
    CHECK(!v.fit && v.zoom==750 && v.x==80 && v.y==50);
    iv_view_zoom(&v,2000);
    CHECK(v.x==480 && v.y==300 && iv_view_width(&v)==1280);
    iv_view_scroll(&v,-1,100000);
    CHECK(v.x==0 && v.y==600);
    iv_view_resize(&v,2000,2000);
    CHECK(v.x==0 && v.y==0 && v.zoom==2000);
    v=(struct iv_view){5,3,20,10,1000,1,0,0};
    iv_view_resize(&v,20,10); iv_view_zoom(&v,16000);
    CHECK(iv_view_width(&v)==80 && iv_view_height(&v)==48 && v.x==30 && v.y==19);
    iv_view_zoom(&v,1); CHECK(v.zoom==125 && iv_view_width(&v)==1 && iv_view_height(&v)==1);
    iv_view_zoom(&v,99999); CHECK(v.zoom==16000);
    v=(struct iv_view){4096,4096,0,0,1000,1,0,0};
    iv_view_resize(&v,319,199);
    CHECK(iv_view_width(&v)<=319 && iv_view_height(&v)<=199 && v.zoom==48);
    iv_view_step(&v,1); CHECK(v.zoom==125 && !v.fit);
    for (int i=0;i<20;++i) iv_view_step(&v,1);
    CHECK(v.zoom==16000 && iv_view_width(&v)==65536);
    for (int i=0;i<20;++i) iv_view_step(&v,-1);
    CHECK(v.zoom==125);
    v.fit=1;iv_view_resize(&v,4096,4096);CHECK(v.zoom==1000 && v.x==0 && v.y==0);
    CHECK(iv_image_name("Photo.JPEG") && iv_image_name("x.JpG") && iv_image_name("x.PNG") && iv_image_name("x.bmp"));
    CHECK(!iv_image_name("x.png.bak") && !iv_image_name("x.gif") && !iv_image_name("image"));
    CHECK(iv_name_compare("a.png","B.bmp")<0 && iv_name_compare("PHOTO.JPEG","photo.jpeg")==0);
    if (failures) return 1;
    puts("IMGVIEW VIEW TEST PASS"); return 0;
}
