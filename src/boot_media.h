/* SPDX-License-Identifier: MIT - bounded boot-volume classification. */
#pragma once
#include "core.h"
/* 0 unsupported, 1 legacy removable USB/SD, 2 GPT partition on SATA/NVMe.
 * GPT here identifies the partition path, not its partition-type GUID. The
 * installer must separately verify an ESP; no partition is modified here. */
static UINTN GuBootMedia(const UINT8 *path,UINTN cap) {
    UINTN length=0,off=0,hd=0;BOOLEAN multi=FALSE,removable=FALSE,disk=FALSE,gpt=FALSE;
    if(!path||!DpMeasure(path,cap,&length,&multi)||multi)return 0;
    while(off+4<length) {
        UINTN n=Read16(path+off+2);const UINT8 *p=path+off;
        if(p[0]==3) {
            /* Reject network transports even if a later HD node exists. */
            if(p[1]==0x0b||p[1]==0x0c||p[1]==0x0d||p[1]==0x13||p[1]==0x14||p[1]==0x18||p[1]==0x1c)return 0;
            if((p[1]==5&&n==6)||(p[1]==0x1a&&n==5))removable=TRUE;
            if((p[1]==0x12&&n==10)||(p[1]==0x17&&n==16))disk=TRUE;
        }
        if(p[0]==4) {
            UINT8 signature=0;
            if(p[1]!=1||n!=42||++hd!=1)return 0;
            for(UINTN i=24;i<40;i++)signature|=p[i];
            gpt=(BOOLEAN)(p[40]==2&&p[41]==2&&Read32(p+4)&&Read64(p+16)&&signature);
        }
        off+=n;
    }
    return removable?1:(disk&&hd==1&&gpt?2:0);
}
