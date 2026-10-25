/* SPDX-License-Identifier: MIT - no device or filesystem access. */
#include "../src/boot_media.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL line %d\n",__LINE__);exit(1);}}while(0)
int main(void) {
    UINT8 p[128]={3,0x17,16,0};UINTN n=62;
    p[16]=4;p[17]=1;p[18]=42;p[20]=1;p[32]=1;p[40]=1;p[56]=2;p[57]=2;
    p[58]=0x7f;p[59]=0xff;p[60]=4;
    CHECK(GuBootMedia(p,n)==2);
    for(UINTN size=0;size<n;size++)CHECK(GuBootMedia(p,size)==0);
    p[57]=1;CHECK(!GuBootMedia(p,n));p[57]=2;
    p[56]=1;CHECK(!GuBootMedia(p,n));p[56]=2;
    p[40]=0;CHECK(!GuBootMedia(p,n));p[40]=1;
    p[32]=0;CHECK(!GuBootMedia(p,n));p[32]=1;
    p[20]=0;CHECK(!GuBootMedia(p,n));p[20]=1;
    p[1]=0x0b;CHECK(!GuBootMedia(p,n));p[1]=0x17;
    p[59]=1;CHECK(!GuBootMedia(p,n));p[59]=0xff;
    p[18]=0xff;CHECK(!GuBootMedia(p,n));p[18]=42;
    UINT8 sata[10]={3,0x12,10,0};UINT8 q[64]={0};
    memcpy(q,sata,10);memcpy(q+10,p+16,46);CHECK(GuBootMedia(q,56)==2);
    UINT8 usb[]={3,5,6,0,0,0,0x7f,0xff,4,0};
    UINT8 sd[]={3,0x1a,5,0,0,0x7f,0xff,4,0};
    CHECK(GuBootMedia(usb,sizeof(usb))==1);CHECK(GuBootMedia(sd,sizeof(sd))==1);
    memset(q,0,sizeof(q));memcpy(q,usb,6);memcpy(q+6,p+16,46);
    q[46]=1;q[47]=1;CHECK(GuBootMedia(q,52)==1); /* Legacy USB MBR. */
    UINT8 no_disk[]={1,1,6,0,0,2,0x7f,0xff,4,0};
    CHECK(!GuBootMedia(no_disk,sizeof(no_disk))&&!GuBootMedia(NULL,0));
    printf("PASS: %u boot-media checks; USB/SD, GPT SATA/NVMe, malformed/network paths\n",checks);
    return 0;
}
