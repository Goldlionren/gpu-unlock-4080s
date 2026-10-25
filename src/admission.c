/* SPDX-License-Identifier: MIT
 * Non-preempting BY_DRIVER reservation, NOT EXCLUSIVE. EXCLUSIVE may implicitly
 * stop another driver. No GPU/bridge writes occur in this module.
 * This guards a single captured NUC topology, not arbitrary Ada machines.
 */
#include "admission.h"
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
static EFI_GUID PciId=EFI_PCI_IO_PROTOCOL_GUID,PathId=EFI_DEVICE_PATH_PROTOCOL_GUID,GopId=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static const UINT8 ExpectedTargetPath[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,1,1,6,0,0,1,1,1,6,0,0,0,0x7f,0xff,4,0};
static const UINT8 IntelParentPath[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,1,1,6,0,0,2,0x7f,0xff,4,0};
static EFI_HANDLE Target,Bridge;
static EFI_PCI_IO_PROTOCOL *BridgePci;
static UINT32 BridgeConfig[64];
static UINT64 BridgeAttributes;
static BOOLEAN Reserved;
static EFI_PCI_IO_PROTOCOL *TargetPci;
BOOLEAN Stage2ReservationHeld(void){return Reserved;}
static EFI_STATUS Path(EFI_BOOT_SERVICES *bs,EFI_HANDLE h,UINT8 **p,UINTN *n) {
    BOOLEAN multi=FALSE;EFI_STATUS s;*p=NULL;*n=0;
    s=bs->HandleProtocol(h,&PathId,(VOID **)p);
    if(EFI_ERROR(s)||!*p||!DpMeasure(*p,PROBE_DP_MAX,n,&multi)||multi)return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
static EFI_STATUS IntelIdentity(EFI_BOOT_SERVICES *bs) {
    EFI_HANDLE *hs=NULL;UINTN count=0,i,matched=0;EFI_STATUS s;
    s=bs->LocateHandleBuffer(ByProtocol,&PciId,NULL,&count,&hs);
    if(EFI_ERROR(s)||!hs||count>512){if(hs)bs->FreePool(hs);return EFI_UNSUPPORTED;}
    for(i=0;i<count;i++) {
        UINT8 *path=NULL,*c;UINTN n=0,segment=0,bus=0,device=0,function=0;
        UINT32 words[16];EFI_PCI_IO_PROTOCOL *pci=NULL;
        if(EFI_ERROR(Path(bs,hs[i],&path,&n))||n!=sizeof(IntelParentPath)||!BytesEqual(path,IntelParentPath,n))continue;
        matched++;
        s=bs->HandleProtocol(hs[i],&PciId,(VOID **)&pci);
        if(EFI_ERROR(s)||!pci||!pci->Pci.Read||!pci->GetLocation){s=EFI_UNSUPPORTED;break;}
        s=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,16,words);if(EFI_ERROR(s))break;c=(UINT8 *)words;
        if(Read16(c)!=0x8086||Read16(c+2)!=0x4680||c[11]!=3||(c[14]&0x7f)!=0){s=EFI_UNSUPPORTED;break;}
        s=pci->GetLocation(pci,&segment,&bus,&device,&function);
        if(EFI_ERROR(s)||segment!=0||bus!=0||device!=2||function!=0){s=EFI_UNSUPPORTED;break;}
    }
    if(EFI_ERROR(bs->FreePool(hs)))return EFI_DEVICE_ERROR;
    if(EFI_ERROR(s))return s;
    return matched==1?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS IntelDisplay(EFI_BOOT_SERVICES *bs) {
    EFI_HANDLE *hs=NULL;UINTN count=0,i,n=0;UINT8 *path=NULL;EFI_STATUS s;
    s=bs->LocateHandleBuffer(ByProtocol,&GopId,NULL,&count,&hs);
    if(EFI_ERROR(s)||!hs||count!=1){if(hs)bs->FreePool(hs);return EFI_UNSUPPORTED;}
    for(i=0;i<count;i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;
        s=Path(bs,hs[i],&path,&n);
        if(EFI_ERROR(s)||!DpParent(IntelParentPath,sizeof(IntelParentPath),path,n)){s=EFI_UNSUPPORTED;break;}
        s=bs->HandleProtocol(hs[i],&GopId,(VOID **)&g);
        if(EFI_ERROR(s)||!g||!g->Mode||!g->Mode->Info||
           g->Mode->SizeOfInfo<sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION)||
           !g->Mode->FrameBufferBase||!g->Mode->FrameBufferSize){s=EFI_UNSUPPORTED;break;}
    }
    if(EFI_ERROR(bs->FreePool(hs)))return EFI_DEVICE_ERROR;
    if(EFI_ERROR(s))return s;
    return IntelIdentity(bs);
}
static EFI_STATUS NoOwners(EFI_BOOT_SERVICES *bs,EFI_HANDLE image,BOOLEAN own) {
    EFI_HANDLE *hs=NULL;UINTN count=0,i,related=0,self_opens=0;EFI_STATUS s;
    s=bs->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&hs);
    if(EFI_ERROR(s)||!hs||!count||count>4096){if(hs)bs->FreePool(hs);return EFI_UNSUPPORTED;}
    for(i=0;i<count;i++) {
        UINT8 *path=NULL;UINTN n=0,j,np=0;EFI_GUID **ps=NULL;
        EFI_STATUS found=Path(bs,hs[i],&path,&n);
        if(hs[i]!=Target&&(EFI_ERROR(found)||!DpParent(ExpectedTargetPath,sizeof(ExpectedTargetPath),path,n)))continue;
        if(hs[i]!=Target||EFI_ERROR(found)||n!=sizeof(ExpectedTargetPath)||!BytesEqual(path,ExpectedTargetPath,n)){s=EFI_ACCESS_DENIED;break;}
        related++;
        s=bs->ProtocolsPerHandle(hs[i],&ps,&np);
        if(EFI_ERROR(s)||!ps||!np||np>64){if(ps)bs->FreePool(ps);s=EFI_UNSUPPORTED;break;}
        for(j=0;j<np;j++) {
            EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN ne=0,k;
            if(!ps[j]){s=EFI_UNSUPPORTED;break;}
            s=bs->OpenProtocolInformation(hs[i],ps[j],&entries,&ne);
            if(EFI_ERROR(s)||(ne&&!entries)||ne>128){if(entries)bs->FreePool(entries);s=EFI_UNSUPPORTED;break;}
            for(k=0;k<ne;k++) {
                UINT32 a=entries[k].Attributes;
                if(a==EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL||a==EFI_OPEN_PROTOCOL_GET_PROTOCOL||a==EFI_OPEN_PROTOCOL_TEST_PROTOCOL)continue;
                if(own&&BytesEqual(ps[j],&PciId,16)&&a==EFI_OPEN_PROTOCOL_BY_DRIVER&&
                   entries[k].AgentHandle==image&&entries[k].ControllerHandle==Target&&entries[k].OpenCount==1){self_opens++;continue;}
                s=EFI_ACCESS_DENIED;break;
            }
            if(entries&&EFI_ERROR(bs->FreePool(entries)))s=EFI_DEVICE_ERROR;
            if(EFI_ERROR(s))break;
        }
        if(EFI_ERROR(bs->FreePool(ps)))s=EFI_DEVICE_ERROR;
        if(EFI_ERROR(s))break;
    }
    if(EFI_ERROR(bs->FreePool(hs)))return EFI_DEVICE_ERROR;
    if(EFI_ERROR(s))return s;
    return related==1&&self_opens==(own?1u:0u)?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static EFI_STATUS Upstream(EFI_BOOT_SERVICES *bs,BOOLEAN save) {
    EFI_HANDLE *hs=NULL;UINTN count=0,i,matched=0;EFI_STATUS s;
    s=bs->LocateHandleBuffer(ByProtocol,&PciId,NULL,&count,&hs);
    if(EFI_ERROR(s)||!hs||count>512){if(hs)bs->FreePool(hs);return EFI_UNSUPPORTED;}
    for(i=0;i<count;i++) {
        UINT8 *path=NULL,*c;UINTN n=0;UINT32 words[64];EFI_PCI_IO_PROTOCOL *pci=NULL;UINT64 attrs=0,base,limit;
        if(EFI_ERROR(Path(bs,hs[i],&path,&n))||n>=sizeof(ExpectedTargetPath)||!DpParent(path,n,ExpectedTargetPath,sizeof(ExpectedTargetPath)))continue;
        s=bs->HandleProtocol(hs[i],&PciId,(VOID **)&pci);
        if(EFI_ERROR(s)||!pci||!pci->Pci.Read||!pci->Attributes){s=EFI_UNSUPPORTED;break;}
        s=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,64,words);if(EFI_ERROR(s))break;c=(UINT8 *)words;
        if((c[14]&0x7f)!=1||c[11]!=6||c[10]!=4||(Read16(c+4)&2)==0||c[0x19]>1||c[0x1a]<1){s=EFI_UNSUPPORTED;break;}
        base=(UINT64)(Read16(c+0x20)&0xfff0u)<<16;limit=((UINT64)(Read16(c+0x22)&0xfff0u)<<16)|0xfffffu;
        if(base>0x83000000u||limit<0x83ffffffu){s=EFI_UNSUPPORTED;break;}
        s=pci->Attributes(pci,EfiPciIoAttributeOperationGet,0,&attrs);if(EFI_ERROR(s))break;
        if(!(attrs&EFI_PCI_IO_ATTRIBUTE_MEMORY)){s=EFI_UNSUPPORTED;break;}
        matched++;
        if(save){UINTN j;Bridge=hs[i];BridgePci=pci;BridgeAttributes=attrs;for(j=0;j<64;j++)BridgeConfig[j]=words[j];}
        else if(hs[i]!=Bridge||pci!=BridgePci||attrs!=BridgeAttributes||!BytesEqual(words,BridgeConfig,sizeof(words))){s=EFI_NOT_READY;break;}
    }
    if(EFI_ERROR(bs->FreePool(hs)))return EFI_DEVICE_ERROR;
    if(EFI_ERROR(s))return s;
    return matched==1?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static void Result(TEXT *t,const CHAR8 *reason,EFI_STATUS status) {
    TextPut(t,"{\"reason\":\"");TextPut(t,reason);TextPut(t,"\",\"status\":");TextHex(t,status,16);
    TextPut(t,",\"by_driver_reservation_held\":");TextPut(t,Reserved?"true":"false");TextPut(t,"}");
}
EFI_STATUS Stage2Reserve(TEXT *t,EFI_HANDLE image,EFI_SYSTEM_TABLE *st,EFI_HANDLE target,EFI_PCI_IO_PROTOCOL *pci) {
    EFI_BOOT_SERVICES *bs=st?st->BootServices:NULL;EFI_STATUS s;VOID *opened=NULL;UINT8 *path=NULL;UINTN n=0;const CHAR8 *reason="missing_services";
    if(Reserved){Result(t,"already_reserved",EFI_ALREADY_STARTED);return EFI_ALREADY_STARTED;}
    Target=target;TargetPci=pci;Bridge=NULL;BridgePci=NULL;
    if(!bs||!image||!target||!pci||!bs->LocateHandleBuffer||!bs->HandleProtocol||!bs->FreePool||
       !bs->ProtocolsPerHandle||!bs->OpenProtocolInformation||!bs->OpenProtocol||!bs->CloseProtocol){s=EFI_UNSUPPORTED;goto done;}
    reason="target_path";s=Path(bs,target,&path,&n);
    if(EFI_ERROR(s)||n!=sizeof(ExpectedTargetPath)||!BytesEqual(path,ExpectedTargetPath,n)){s=EFI_UNSUPPORTED;goto done;}
    reason="intel_display";s=IntelDisplay(bs);if(EFI_ERROR(s))goto done;
    reason="owners_before";s=NoOwners(bs,image,FALSE);if(EFI_ERROR(s))goto done;
    reason="upstream_bridge";s=Upstream(bs,TRUE);if(EFI_ERROR(s))goto done;
    reason="reserve_by_driver";
    s=bs->OpenProtocol(target,&PciId,&opened,image,target,EFI_OPEN_PROTOCOL_BY_DRIVER);
    if(EFI_ERROR(s))goto done;Reserved=TRUE;
    if(opened!=pci){s=EFI_COMPROMISED_DATA;goto done;}
    reason="owners_after";s=NoOwners(bs,image,TRUE);if(EFI_ERROR(s))goto done;
    reason="bridge_after_reserve";s=Upstream(bs,FALSE);if(EFI_ERROR(s))goto done;
    reason="display_after_reserve";s=IntelDisplay(bs);if(EFI_ERROR(s))goto done;
    reason="admitted";
done:
    Result(t,reason,s);return s;
}
EFI_STATUS Stage2Release(TEXT *t,EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs=st->BootServices;EFI_STATUS bridge=EFI_SUCCESS,owners=EFI_SUCCESS,display=EFI_SUCCESS,close=EFI_SUCCESS;
    if(Reserved){bridge=Upstream(bs,FALSE);owners=NoOwners(bs,image,TRUE);display=IntelDisplay(bs);
        close=bs->CloseProtocol(Target,&PciId,image,Target);if(!EFI_ERROR(close))Reserved=FALSE;}
    TextPut(t,"{\"bridge_status\":");TextHex(t,bridge,16);TextPut(t,",\"owners_status\":");TextHex(t,owners,16);
    TextPut(t,",\"display_status\":");TextHex(t,display,16);TextPut(t,",\"close_status\":");TextHex(t,close,16);
    TextPut(t,",\"reservation_released\":");TextPut(t,Reserved?"false":"true");TextPut(t,"}");
    if(EFI_ERROR(bridge)||EFI_ERROR(owners)||EFI_ERROR(display)||EFI_ERROR(close))return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}
EFI_STATUS Stage2Recheck(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_STATUS s;VOID *current=NULL;EFI_BOOT_SERVICES *bs=st->BootServices;
    if(!Reserved)return EFI_ACCESS_DENIED;
    s=bs->HandleProtocol(Target,&PciId,&current);
    if(EFI_ERROR(s)||current!=TargetPci)return EFI_COMPROMISED_DATA;
    s=NoOwners(bs,image,TRUE);if(EFI_ERROR(s))return s;
    s=Upstream(bs,FALSE);if(EFI_ERROR(s))return s;
    return IntelDisplay(bs);
}
