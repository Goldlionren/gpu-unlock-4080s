/* SPDX-License-Identifier: GPL-2.0-only
 * Host-only harness for unchanged reference layout_610.c. No device APIs. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "layout_610.h"
int main(int argc, char **argv) {
    struct gpu_unlock_layout_in in={0};struct gpu_unlock_layout_out out={0};
    unsigned long long values[9];int i,rc;char *end;
    if(argc!=10)return 2;
    for(i=0;i<9;i++){errno=0;end=NULL;values[i]=strtoull(argv[i+1],&end,0);if(errno||!end||*end)return 3;}
    in.usable_fb=values[0];in.vga_base=values[1];in.mmu_lock_valid=values[2]!=0;
    in.mmu_lock_low=values[3];in.frts_size=values[4];in.boot_size=values[5];
    in.image_size=values[6];in.nonwpr_size=values[7];in.heap_request=values[8];
    rc=gpu_unlock_layout(&in,&out);printf("{\"return_code\":%d,\"output\":{",rc);
#define PRINT_FIELD(name) printf("\"" #name "\":%llu,",(unsigned long long)out.name)
    PRINT_FIELD(reserved_top);PRINT_FIELD(wpr_end);PRINT_FIELD(frts_offset);PRINT_FIELD(boot_offset);
    PRINT_FIELD(gsp_offset);PRINT_FIELD(heap_offset);PRINT_FIELD(heap_size);PRINT_FIELD(wpr_start);
    printf("\"nonwpr_start\":%llu}}\n",(unsigned long long)out.nonwpr_start);
    return rc?1:0;
}
