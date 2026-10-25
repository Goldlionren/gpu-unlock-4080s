/* SPDX-License-Identifier: MIT
 * Local RTX4080 SUPER 16GB integration; hardware profile from CAP_STAGE2_0002.
 * No display-driver disconnect/reconnect, boot-variable writes, or VBIOS flash.
 */
#include "app_io.h"
#include "compat/compat.h"
#include "checks.h"
#include "sha256.h"
#include "prom_profile.h"
#include "admission.h"
#include "vendor/unlock_610.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#ifndef UNLOCK_BUILD_ID
#error Build through Build.ps1
#endif
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c}};
static EFI_SYSTEM_TABLE *Table;
static EFI_HANDLE ImageHandle,AppVolume,WindowsImage;
static VOID *WindowsPath;
static UINT32 EntryConfig[64];
static UINT32 FinalConfig[64];
static EFI_STATUS EntryRestoreStatus,FinalConfigStatus;
static BOOLEAN EntryDiagnostics;
static BOOLEAN FinalRecoveryVerified,StatusClearAccepted,FinalConfigAccepted;
static BOOLEAN EntryChanged;
static CHAR8 JsonBuffer[4096];
/* Explicit relocation also keeps an EFI loader relocation directory. */
static CHAR8 * volatile Anchor=JsonBuffer;
static const char *Phase="init";

static EFI_STATUS ConfigRead(UINT32 words[64]) {
    return Gu.pci->Pci.Read(Gu.pci,EfiPciIoWidthUint32,0,64,words);
}
static BOOLEAN DisplayCheck(VOID) {return !EFI_ERROR(Stage2Recheck(ImageHandle,Table));}
static EFI_STATUS Target(VOID) {
    EFI_HANDLE *handles=NULL;UINTN count=0,found=0;EFI_STATUS s;
    s=Gu.bs->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
    if(EFI_ERROR(s)||!handles||count>512){if(handles)Gu.bs->FreePool(handles);return EFI_UNSUPPORTED;}
    for(UINTN i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *p=NULL;UINT32 cfg[64];UINTN seg,bus,dev,fun;
        s=Gu.bs->HandleProtocol(handles[i],&PciGuid,(VOID**)&p);
        if(EFI_ERROR(s)||!p||!p->Pci.Read||!p->GetLocation){s=EFI_DEVICE_ERROR;break;}
        s=p->Pci.Read(p,EfiPciIoWidthUint32,0,64,cfg);if(EFI_ERROR(s))break;
        if(cfg[0]!=GU_BOARD_ID)continue;
        found++;s=p->GetLocation(p,&seg,&bus,&dev,&fun);if(EFI_ERROR(s))break;
        if(!GuTargetConfig(cfg,seg,bus,dev,fun)||!p->Pci.Write||!p->Mem.Read||!p->Mem.Write||
           !p->Attributes||!p->GetBarAttributes||!p->AllocateBuffer||!p->FreeBuffer||!p->Map||!p->Unmap||!p->Flush){s=EFI_UNSUPPORTED;break;}
        Gu.pci=p;Gu.handle=handles[i];memcpy(EntryConfig,cfg,sizeof(cfg));
    }
    if(EFI_ERROR(Gu.bs->FreePool(handles)))return EFI_DEVICE_ERROR;
    return !EFI_ERROR(s)&&found==1?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS ConfigCheck(UINT16 expected) {
    UINT32 cfg[64],checked[64];UINT64 attrs=~0ULL;GU_CAPS caps;
    EFI_STATUS s=ConfigRead(cfg);if(EFI_ERROR(s))return s;
    if((UINT16)cfg[1]!=expected||cfg[0]!=GU_BOARD_ID||cfg[11]!=GU_BOARD_SUBSYSTEM||
       (cfg[2]&255)!=0xa1||cfg[4]!=0x83000000||cfg[5]!=0xc||cfg[6]!=0x60||
       cfg[7]!=0xc||cfg[8]!=0x64||cfg[9]!=0x3001||cfg[12]!=0x84000000)return EFI_UNSUPPORTED;
    /* Reference capability parser requires MEM set; normalize only the local
     * copy, never hardware, to validate the entry command-zero snapshot. */
    memcpy(checked,cfg,sizeof(cfg));checked[1]=(checked[1]&0xffff0000u)|2;
    if(!GuCheckConfig(checked,0,1,0,0,&caps))return EFI_UNSUPPORTED;
    if((Read16((UINT8*)cfg+caps.pcie+10)&0x20)!=0)return EFI_NOT_READY;
    s=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attrs);
    if(EFI_ERROR(s)||attrs!=0)return EFI_UNSUPPORTED;
    Gu.pm_cap=caps.pm;Gu.pcie_cap=caps.pcie;Gu.pcie_version=caps.pcie_version;
    Gu.msi_cap=caps.msi;Gu.msix_cap=caps.msix;
    return EFI_SUCCESS;
}
static EFI_STATUS BarCheck(VOID) {
    VOID *resource=NULL;UINT64 support=0;EFI_STATUS s=Gu.pci->GetBarAttributes(Gu.pci,0,&support,&resource);
    BOOLEAN good=FALSE;
    if(!EFI_ERROR(s)&&resource){const UINT8 *r=resource;
        good=(BOOLEAN)(r[0]==0x8a&&Read16(r+1)==43&&r[3]==0&&Read64(r+6)==32&&
            Read64(r+14)==0x83000000&&Read64(r+22)==0x83ffffff&&Read64(r+30)==0&&Read64(r+38)==0x1000000&&r[46]==0x79);}
    if(resource&&EFI_ERROR(Gu.bs->FreePool(resource)))return EFI_DEVICE_ERROR;
    if(!good)return EFI_UNSUPPORTED;Gu.bar_base=0x83000000;Gu.bar_size=0x1000000;return EFI_SUCCESS;
}
static EFI_STATUS EnableMemory(VOID) {
    UINT32 cfg[64];UINT16 command=2;EFI_STATUS s;
    s=ConfigCheck(0);if(EFI_ERROR(s))return s;
    s=ConfigRead(cfg);if(EFI_ERROR(s)||memcmp(cfg,EntryConfig,sizeof(cfg)))return EFI_NOT_READY;
    if(!DisplayCheck())return EFI_ACCESS_DENIED;
    GuLogText("PCI MEM intent: endpoint command Uint16 0->2; Attributes cache remains 0.\n");
    if(!GuLogReady())return EFI_DEVICE_ERROR;
    EntryChanged=TRUE; /* before an API that may partially apply then fail */
    s=Gu.pci->Pci.Write(Gu.pci,EfiPciIoWidthUint16,4,1,&command);
    if(EFI_ERROR(s))return s;
    s=ConfigRead(cfg);if(EFI_ERROR(s)||(UINT16)cfg[1]!=2)return EFI_DEVICE_ERROR;
    cfg[1]&=0xffff0000u;if(memcmp(cfg,EntryConfig,sizeof(cfg)))return EFI_DEVICE_ERROR;
    return ConfigCheck(2);
}
/* This board has reported all three initial SS0 values. Only the initial
 * admission policy varies; the fixed unlock target and recovery gates do not.
 * These observations alone are not hardware validation of the new paths. */
static BOOLEAN ColdSs0Allowed(UINT32 value) {
    return (BOOLEAN)(value==0x64113035||value==0x64113036||value==0x64113037);
}
static EFI_STATUS ColdAndProm(VOID) {
    static const UINT32 offsets[]={0,0x1fa828,0x1fa824,0x118128,0x118234,0x110100,0x1fa7cc,0x1fa7c4,
        0x9a0148,0x823804,0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0};
    static const UINT32 values[]={0x193000a1,0,0x1ffffe00,0x8b8f,0x3ff,0x10,0x4cb8f,0x4cb8f,
        0xffffff0f,0xffffff8f,0x233,0x410080,0x64113035,2,0x3ff8,1,0x45cb8f,0x100000,0x3cf7ff0,1,1};
    BOOLEAN profileGood=TRUE,otherMismatch=FALSE;UINT32 firstSs0=0,firstSs1=0;
    GuLogText("cold diagnostics revision=2; initial SS0 allowlist=0x64113035,0x64113036,0x64113037; other admission and unlock targets unchanged\n");
    for(UINTN i=0;i<ARRAY_SIZE(offsets);i++) {
        UINT32 value=GuRead32(offsets[i]);
        GuLog("cold reg=%#x actual=%#x expected=%#x\n",offsets[i],value,values[i]);
        if(Gu.fault)return EFI_NOT_READY;
        if(offsets[i]==0x82381c)firstSs0=value;
        if(offsets[i]==0x823820)firstSs1=value;
        if(value!=values[i]) {
            if(offsets[i]!=0x82381c||!ColdSs0Allowed(value))profileGood=FALSE;
            if(offsets[i]!=0x82381c)otherMismatch=TRUE;
            if(i==0)return EFI_NOT_READY; /* Never probe further on a chip identity mismatch. */
        }
    }
    UINT32 secondSs0=GuRead32(0x82381c),secondSs1=GuRead32(0x823820);
    GuLog("cold selectors repeat: SS0 first=%#x second=%#x SS1 first=%#x second=%#x stable=%u\n",
        firstSs0,secondSs0,firstSs1,secondSs1,firstSs0==secondSs0&&firstSs1==secondSs1);
    if(Gu.fault)return EFI_NOT_READY;
    if(!ColdSs0Allowed(secondSs0)||firstSs0!=secondSs0)profileGood=FALSE;
    if(secondSs1!=2||firstSs1!=secondSs1){profileGood=FALSE;otherMismatch=TRUE;}
    if(otherMismatch){GuLogText("cold profile rejected: non-SS0 mismatch; PROM not read; firmware not started\n");return EFI_NOT_READY;}
    UINT8 *a=GuAlloc(1048576),*b=GuAlloc(1048576);BOOLEAN good=FALSE;
    if(a&&b) {
        GuCopyFromIo(a,(VOID*)(UINTN)(Gu.bar_base+GU_PROM_BAR_OFFSET),1048576);
        GuCopyFromIo(b,(VOID*)(UINTN)(Gu.bar_base+GU_PROM_BAR_OFFSET),1048576);
        good=(BOOLEAN)(!Gu.fault&&!memcmp(a,b,1048576)&&GuHashMatches(a,1048576,GU_PROM_CAPTURE_SHA256)&&
            GuHashMatches(a,GU_PROM_VIEW_BYTES,GU_PROM_VIEW_SHA256));
    }
    GuFree(a);GuFree(b);GuLog("PROM double-read and exact board hash matched=%u\n",good);
    if(!good||Gu.fault)return EFI_CRC_ERROR;
    UINT32 finalSs0=GuRead32(0x82381c),finalSs1=GuRead32(0x823820);
    GuLog("cold selectors post-PROM: SS0=%#x SS1=%#x stable=%u allowed=%u\n",finalSs0,finalSs1,
        finalSs0==firstSs0&&finalSs1==firstSs1,ColdSs0Allowed(finalSs0)&&finalSs1==2);
    if(Gu.fault||finalSs0!=firstSs0||!ColdSs0Allowed(finalSs0)||finalSs1!=firstSs1||finalSs1!=2)return EFI_NOT_READY;
    if(!profileGood){GuLogText("cold profile rejected: SS0 mismatch; remaining profile and PROM verified; firmware not started\n");return EFI_NOT_READY;}
    GuLog("cold profile accepted: initial_SS0=%#x initial_SS1=%#x observed_variant=%u PROM_verified=1\n",
        firstSs0,firstSs1,firstSs0!=0x64113035);
    return EFI_SUCCESS;
}
static EFI_STATUS PreloadWindows(VOID) {
    static const CHAR16 Name[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    EFI_HANDLE *handles=NULL,volume=NULL;UINTN count=0,found=0;EFI_STATUS s;
    EFI_LOADED_IMAGE_PROTOCOL *own=NULL;
    s=Gu.bs->HandleProtocol(ImageHandle,&LoadedGuid,(VOID**)&own);if(EFI_ERROR(s)||!own)return EFI_UNSUPPORTED;
    AppVolume=own->DeviceHandle;
    s=Gu.bs->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,&count,&handles);
    if(EFI_ERROR(s)||!handles||count>128){if(handles)Gu.bs->FreePool(handles);return EFI_UNSUPPORTED;}
    for(UINTN i=0;i<count;i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_FILE_PROTOCOL *root=NULL,*file=NULL;
        /* A disk installation can share the Windows ESP. Still require one
         * unambiguous internal Windows loader and validate its loaded volume. */
        if(handles[i]==AppVolume&&!GuInternalBoot)continue;
        s=Gu.bs->HandleProtocol(handles[i],&FsGuid,(VOID**)&fs);if(EFI_ERROR(s)||!fs)continue;
        s=fs->OpenVolume(fs,&root);if(EFI_ERROR(s)||!root)continue;
        s=root->Open(root,&file,(CHAR16*)Name,EFI_FILE_MODE_READ,0);
        EFI_STATUS closed=root->Close(root);if(EFI_ERROR(closed)){if(file)file->Close(file);s=closed;goto done;}
        if(!EFI_ERROR(s)&&file) {found++;volume=handles[i];s=file->Close(file);if(EFI_ERROR(s))goto done;}
        else if(s!=EFI_NOT_FOUND){s=EFI_UNSUPPORTED;goto done;}
    }
    s=found==1?EFI_SUCCESS:EFI_UNSUPPORTED;
done:
    if(EFI_ERROR(Gu.bs->FreePool(handles)))return EFI_DEVICE_ERROR;
    if(EFI_ERROR(s))return s;
    VOID *dp=NULL;UINTN length=0;BOOLEAN multi=FALSE;
    s=Gu.bs->HandleProtocol(volume,&PathGuid,&dp);
    if(EFI_ERROR(s)||!dp||!DpMeasure(dp,4096,&length,&multi)||multi||DpHasUsb(dp,length))return EFI_UNSUPPORTED;
    UINTN node=4+sizeof(Name),total=length-4+node+4;
    if(total>4096)return EFI_UNSUPPORTED;
    UINT8 *path=GuAlloc(total);if(!path)return EFI_OUT_OF_RESOURCES;
    memcpy(path,dp,length-4);path[length-4]=MEDIA_DEVICE_PATH;path[length-3]=MEDIA_FILEPATH_DP;
    path[length-2]=(UINT8)node;path[length-1]=(UINT8)(node>>8);memcpy(path+length,Name,sizeof(Name));
    path[total-4]=END_DEVICE_PATH_TYPE;path[total-3]=END_ENTIRE_DEVICE_PATH_SUBTYPE;path[total-2]=4;path[total-1]=0;
    WindowsPath=path;
    s=Gu.bs->LoadImage(FALSE,ImageHandle,(EFI_DEVICE_PATH_PROTOCOL*)path,NULL,0,&WindowsImage);
    if(!EFI_ERROR(s)) {
        EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
        if(!WindowsImage)s=EFI_COMPROMISED_DATA;
        else {
            s=Gu.bs->HandleProtocol(WindowsImage,&LoadedGuid,(VOID**)&loaded);
            if(!EFI_ERROR(s)&&(!loaded||loaded->DeviceHandle!=volume||loaded->ImageCodeType!=EfiLoaderCode))s=EFI_COMPROMISED_DATA;
        }
    }
    GuLog("Windows boot manager preload: one internal candidate, status=%#llx\n",(unsigned long long)s);
    return s;
}
static EFI_STATUS Arm(VOID) {
    EFI_INPUT_KEY key;EFI_STATUS s;UINTN i;
    if(!Table->ConIn||!Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;
    for(i=0;i<64;i++){s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);if(s==EFI_NOT_READY)break;if(EFI_ERROR(s))return s;}
    if(i==64)return EFI_NOT_READY;
    GuLogText("READY: press U within 30 seconds for ONE experimental firmware transaction. ESC cancels.\nNo VBIOS flashing. On STOP or hang physically power OFF; do not reset into Windows.\n");
    if(!GuLogReady())return EFI_DEVICE_ERROR;
    for(i=0;i<300;i++) {
        s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
        if(!EFI_ERROR(s)){if(key.UnicodeChar=='U'||key.UnicodeChar=='u')return EFI_SUCCESS;if(key.ScanCode==0x17)return EFI_ABORTED;}
        else if(s!=EFI_NOT_READY)return s;GuStall(100000);
    }
    return EFI_TIMEOUT;
}
/* Board-specific exception supported by physical RUN03: PCIe capability 0x78,
 * DEVCTL stays 0x2930 while DEVSTA CED/URD records clear from 9 to 0.
 * Only after verified FLR recovery. No status writes or general status mask.
 * Every other byte, including every control bit, must still match entry. */
static BOOLEAN FinalEntryMatches(const UINT32 final[64],const UINT32 entry[64],
                                BOOLEAN verifiedFlr,UINT16 pcie,BOOLEAN *accepted) {
    UINT32 normalized[64];*accepted=FALSE;
    if(!memcmp(final,entry,sizeof(normalized)))return TRUE;
    if(!verifiedFlr||pcie!=0x78||entry[0x80/4]!=0x00092930||final[0x80/4]!=0x00002930)return FALSE;
    memcpy(normalized,final,sizeof(normalized));normalized[0x80/4]=entry[0x80/4];
    if(memcmp(normalized,entry,sizeof(normalized)))return FALSE;
    *accepted=TRUE;return TRUE;
}
static VOID LogEntryDiagnostics(VOID) {
    if(!EntryDiagnostics)return;
    UINTN differences=0;
    GuLog("entry command restore detail: step=%s api_status=%#llx id=%#x before=%#x after=%#x attributes=%#llx\n",
        Gu.entry_restore_step?Gu.entry_restore_step:"unrecorded",(unsigned long long)Gu.entry_restore_api_status,
        Gu.entry_restore_id,Gu.entry_restore_before,Gu.entry_restore_after,(unsigned long long)Gu.entry_restore_attributes);
    if(!EFI_ERROR(FinalConfigStatus)) {
        for(UINTN i=0;i<64;i++)if(FinalConfig[i]!=EntryConfig[i]) {
            differences++;
            GuLog("entry config delta: offset=%#x entry=%#x final=%#x xor=%#x\n",
                (UINT32)i*4,EntryConfig[i],FinalConfig[i],EntryConfig[i]^FinalConfig[i]);
        }
    }
    GuLog("entry restore check: command_status=%#llx config_read_status=%#llx compared=%u differences=%u fault=%u retained=%u\n",
        (unsigned long long)EntryRestoreStatus,(unsigned long long)FinalConfigStatus,!EFI_ERROR(FinalConfigStatus),(unsigned)differences,Gu.fault,Gu.retained);
    GuLog("entry status policy: verified_flr=%u accepted_ced_urd_clear=%u configuration_accepted=%u\n",FinalRecoveryVerified,StatusClearAccepted,FinalConfigAccepted);
    EntryDiagnostics=FALSE;
}
static EFI_STATUS Stop(VOID) {
    /* An earlier failed API may have unexpectedly enabled BME. Containment is
     * raw, identity-checked and allowed with a fault latched. A failure here
     * remains a STOP; DMA buffers are never released on this path. */
    EFI_STATUS contained=EFI_NOT_STARTED;
    BOOLEAN faultBefore=Gu.fault,retainedBefore=Gu.retained,pendingBefore=Gu.master_attributes_pending;
    if(Stage2ReservationHeld()&&Gu.pci&&(EntryChanged||Gu.started||Gu.master_attributes_pending)) {
        VOID *current=NULL;
        contained=Gu.bs->HandleProtocol(Gu.handle,&PciGuid,&current);
        if(!EFI_ERROR(contained))contained=current==Gu.pci?GuContainMasterForStop():EFI_COMPROMISED_DATA;
    }
    Gu.retained=TRUE;
    /* Preserve the original cause without delaying BME containment for USB IO. */
    GuLog("STOP cause: phase=%s fault_before_containment=%u retained_before_containment=%u pending=%u\n",
        Phase,faultBefore,retainedBefore,pendingBefore);
    LogEntryDiagnostics();
    GuLog("STOP phase=%s containment=%#llx started=%u quiesced=%u fault=%u maps=%u\n",Phase,(unsigned long long)contained,Gu.started,Gu.quiesced,Gu.fault,Gu.dma_count);
    GuConsole("Power OFF physically. Do not continue to Windows. Resources retained until power off.\n");
#ifdef GU_APP_HOST_TEST
    return EFI_ABORTED;
#else
    for(;;)Gu.bs->Stall(1000000);
#endif
}
EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_STATUS s=EFI_NOT_STARTED,cleanup=EFI_SUCCESS;BOOLEAN attempted=FALSE;TEXT json;
    struct gpu_unlock_610_shared shared={0};UINT64 dmaMask=0xffffffffULL;
    struct pci_dev device={.dev={.dma_mask=&dmaMask,.coherent_dma_mask=0xffffffffULL},.refs=1};
    if(!st||!st->BootServices||!st->BootServices->Stall||!st->RuntimeServices||!st->RuntimeServices->GetVariable||Anchor!=JsonBuffer)return EFI_INVALID_PARAMETER;
    Table=st;ImageHandle=image;WindowsImage=NULL;WindowsPath=NULL;EntryChanged=FALSE;EntryDiagnostics=FALSE;GuScreenDetached=FALSE;
    FinalRecoveryVerified=StatusClearAccepted=FinalConfigAccepted=FALSE;
    GuInit(st->BootServices,NULL);Gu.image=image;Gu.display_check=DisplayCheck;
    Phase="usb-log";s=GuIoOpen(image,st);if(EFI_ERROR(s))goto finish;
    GuLog("build_id=%s CAP_STAGE2_0002=4c65ad8928ef717eccd4cc59b32b4e0a6a69a85bb143a55055cb2818b59f67ea\n",UNLOCK_BUILD_ID);
    UINT8 secure=1;UINTN size=1;Phase="secure-boot";
    s=st->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,NULL,&size,&secure);
    if(EFI_ERROR(s)||size!=1||secure>1){s=EFI_SECURITY_VIOLATION;goto finish;}
    if(secure) {
        UINT8 setup=1;size=1;
        s=st->RuntimeServices->GetVariable(L"SetupMode",&GlobalGuid,NULL,&size,&setup);
        if(EFI_ERROR(s)||size!=1||setup!=0){s=EFI_SECURITY_VIOLATION;goto finish;}
    }
    /* Firmware performs executable authentication before this entry point.
     * This program never enrolls keys or overrides LoadImage authentication. */
    GuLog("boot policy: secure_boot=%u internal_volume=%u firmware_authentication_not_overridden=1\n",secure,GuInternalBoot);
    Phase="watchdog";s=Gu.bs->SetWatchdogTimer?Gu.bs->SetWatchdogTimer(0,0,0,NULL):EFI_UNSUPPORTED;
    if(EFI_ERROR(s))goto finish;
    Phase="target";s=Target();if(EFI_ERROR(s))goto finish;
    Phase="entry-config";s=ConfigCheck(0);if(EFI_ERROR(s))goto finish;
    Phase="bar";s=BarCheck();if(EFI_ERROR(s))goto finish;
    Phase="assets";s=GuLoadAssets();if(EFI_ERROR(s))goto finish;
    Phase="windows-preload";s=PreloadWindows();if(EFI_ERROR(s))goto finish;
    Phase="arming";s=Arm();if(EFI_ERROR(s))goto finish;
    Phase="admission";TextInit(&json,JsonBuffer,sizeof(JsonBuffer));
    s=Stage2Reserve(&json,image,st,Gu.handle,Gu.pci);Gu.owned=Stage2ReservationHeld();
    GuLog("admission=%s\n",JsonBuffer);if(json.failed)s=EFI_BUFFER_TOO_SMALL;if(EFI_ERROR(s))goto finish;
    Gu.display_ok=DisplayCheck();if(!Gu.display_ok){s=EFI_ACCESS_DENIED;goto finish;}
    Phase="memory-enable";s=EnableMemory();if(EFI_ERROR(s))goto finish;
    Phase="cold-prom-profile";s=ColdAndProm();if(EFI_ERROR(s))goto finish;
    Phase="final-recheck";s=ConfigCheck(2);if(EFI_ERROR(s)||!DisplayCheck()){s=EFI_NOT_READY;goto finish;}
    UINT32 rechecked[64];s=ConfigRead(rechecked);
    if(EFI_ERROR(s))goto finish;rechecked[1]&=0xffff0000u;
    if(memcmp(rechecked,EntryConfig,sizeof(rechecked))){s=EFI_NOT_READY;goto finish;}
    GuLogText("ARMED: one FWSEC/SEC2 transaction. Intent flushed before firmware access.\n");
    if(!GuLogReady()){s=EFI_DEVICE_ERROR;goto finish;}
    Phase="transaction";Gu.armed=TRUE;attempted=TRUE;device.pm_cap=Gu.pm_cap;
    int result=gpu_unlock_610_load_shared(&device,&shared);
    if(!result)result=gpu_unlock_610_unlock(&device,&shared);
    cleanup=GuClearMaster();
    const struct gpu_unlock_610_outcome *outcome=gpu_unlock_610_get_outcome();
    GuLog("transaction result=%d containment=%#llx started=%u quiesced=%u retained=%u maps=%u\n",result,(unsigned long long)cleanup,Gu.started,Gu.quiesced,Gu.retained,Gu.dma_count);
    if(EFI_ERROR(cleanup)||!GuSafeToReturn()||Gu.allocations||(Gu.started&&result))return Stop();
    s=result?EFI_DEVICE_ERROR:EFI_SUCCESS;
    if(!EFI_ERROR(s)&&(!outcome->recovered||!outcome->required_plm_verified||!outcome->selectors_before_flr||!outcome->selectors_after_flr))return Stop();
    if(!EFI_ERROR(s)) {
        UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820),hi=GuRead32(0x1fa828),cpu=GuRead32(0x110100);
        GuLog("final selector/gsp readback: SS0=%#x SS1=%#x WPR2_HI=%#x GSP_CPUCTL=%#x\n",ss0,ss1,hi,cpu);
        if(Gu.fault||ss0!=0x88888888||ss1!=8||(hi>>4)||!(cpu&0x10))return Stop();
        FinalRecoveryVerified=(BOOLEAN)(Gu.flrs==1&&Gu.started&&Gu.quiesced&&!Gu.fault);
    }
    gpu_unlock_610_release_shared(&shared);Gu.armed=FALSE;
finish: ;
    const char *completedPhase=Phase;
    if(EntryChanged) {
        Phase="entry-restore";cleanup=GuRestoreEntryCommandZero();
        EntryRestoreStatus=cleanup;EntryDiagnostics=TRUE;FinalConfigStatus=EFI_NOT_STARTED;
        if(EFI_ERROR(cleanup))return Stop();
        FinalConfigStatus=ConfigRead(FinalConfig);
        if(EFI_ERROR(FinalConfigStatus))return Stop();
        FinalConfigAccepted=FinalEntryMatches(FinalConfig,EntryConfig,FinalRecoveryVerified,Gu.pcie_cap,&StatusClearAccepted);
        if(!FinalConfigAccepted)return Stop();
        LogEntryDiagnostics();
        EntryChanged=FALSE;
    }
    if(Stage2ReservationHeld()) {
        Phase="release";TextInit(&json,JsonBuffer,sizeof(JsonBuffer));cleanup=Stage2Release(&json,image,st);
        Gu.owned=Stage2ReservationHeld();GuLog("release=%s\n",JsonBuffer);
        if(EFI_ERROR(cleanup)||json.failed)return Stop();
    }
    GuLog("FINAL status=%#llx phase=%s attempted=%u firmware_started=%u restored_entry_command=0 performance_validated=0\n",(unsigned long long)s,completedPhase,attempted,Gu.started);
    if(Gu.started&&(!GuLogReady()||!GuSafeToReturn()))return Stop();
    GuReleaseAssets();cleanup=GuIoClose();
    if(EFI_ERROR(cleanup)){if(Gu.started)return Stop();s=cleanup;}
    if(!EFI_ERROR(s)&&attempted) {
        GuConsole("Selector readback and recovery verified. Starting Windows; performance validation remains pending.\n");
        UINTN bytes=0;CHAR16 *data=NULL;Phase="windows-start";
        s=Gu.bs->StartImage(WindowsImage,&bytes,&data);(void)s;(void)data;
        return Stop(); /* Never return to firmware after a hardware-success handoff. */
    }
    if(WindowsImage)Gu.bs->UnloadImage(WindowsImage);
    GuFree(WindowsPath);WindowsPath=NULL;
    if(!attempted&&!strcmp(completedPhase,"cold-prom-profile"))
        GuConsole("Initial GPU profile rejected. Diagnostic logged; firmware transaction NOT started.\n");
    GuConsole(GuInternalBoot?"No successful unlock. Read EFI/GPUUnlock/RUNxx/REPORT.TXT. Returning in 8 seconds.\n":
        "No successful unlock. Read RTX4080S-UNLOCK/RUNxx/REPORT.TXT. Returning in 8 seconds.\n");
    Gu.bs->Stall(8000000);return s;
}
