/* SPDX-License-Identifier: MIT
 * Host-only protocol database mocks. Never executes firmware or hardware I/O.
 */
#include "../src/admission.h"
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Mode,Opens,Closes,Active,ReleasePhase;
static EFI_GUID PciId=EFI_PCI_IO_PROTOCOL_GUID,PathId=EFI_DEVICE_PATH_PROTOCOL_GUID,GopId=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_BOOT_SERVICES Bs;static EFI_SYSTEM_TABLE St;
static EFI_PCI_IO_PROTOCOL TargetPci,BridgePci,IntelPci;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION GopInfo;
static UINT8 BridgeCfg[256],IntelCfg[64];static CHAR8 Output[8192];
#define H(n) ((EFI_HANDLE)(UINTN)(n))
#define TARGET H(1)
#define BRIDGE H(2)
#define INTEL H(3)
#define GOP H(4)
#define IMAGE H(5)
#define CHILD H(6)
#define OTHER H(7)
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"admission FAIL line %d mode %u: %s\n",__LINE__,Mode,#x);exit(1);}}while(0)
enum {OK,INTEL_WRONG_VENDOR,INTEL_WRONG_DEVICE,INTEL_WRONG_CLASS,INTEL_WRONG_BDF,INTEL_READ_FAIL,INTEL_ABSENT,
    GOP_ABSENT,GOP_MULTIPLE,GOP_WRONG_PATH,GOP_BAD_INFO,OWNER_BEFORE,CHILD_PRESENT,INVENTORY_FAIL,
    BRIDGE_MEM_OFF,BRIDGE_CACHE_OFF,BRIDGE_WINDOW_BAD,BRIDGE_ABSENT,OPEN_DENIED,OPEN_WRONG_POINTER,
    OWNER_AFTER,BRIDGE_AFTER,GOP_AFTER,CLOSE_FAIL,BRIDGE_RELEASE,OWNER_RELEASE,GOP_RELEASE};
static const UINT8 TargetPath[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,1,1,6,0,0,1,1,1,6,0,0,0,0x7f,0xff,4,0};
static const UINT8 BridgePath[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,1,1,6,0,0,1,0x7f,0xff,4,0};
static const UINT8 IntelPath[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,1,1,6,0,0,2,0x7f,0xff,4,0};
static UINT8 ChildPath[34];
static BOOLEAN Guid(const EFI_GUID *a,const EFI_GUID *b){return (BOOLEAN)(memcmp(a,b,16)==0);}
static void Put(UINT8 *p,UINT64 v,UINTN n){UINTN i;for(i=0;i<n;i++)p[i]=(UINT8)(v>>(8*i));}
static EFI_STATUS EFIAPI Free(VOID *p){CHECK(p!=NULL);free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out) {
    EFI_HANDLE values[7];UINTN count=0;CHECK(!key&&n&&out);
    if(type==AllHandles){
        CHECK(!guid);if(Mode==INVENTORY_FAIL)return EFI_DEVICE_ERROR;
        values[count++]=TARGET;values[count++]=BRIDGE;values[count++]=INTEL;values[count++]=GOP;values[count++]=IMAGE;
        if(Mode==CHILD_PRESENT)values[count++]=CHILD;
    }else {
        CHECK(type==ByProtocol);
        if(Guid(guid,&PciId)){
            values[count++]=TARGET;if(Mode!=BRIDGE_ABSENT)values[count++]=BRIDGE;
            if(Mode!=INTEL_ABSENT)values[count++]=INTEL;
        }else{
            CHECK(Guid(guid,&GopId));
            if(Mode!=GOP_ABSENT)values[count++]=GOP;
            if(Mode==GOP_MULTIPLE)values[count++]=GOP;
        }
    }
    *n=count;*out=malloc((count?count:1)*sizeof(**out));CHECK(*out!=NULL);
    memcpy(*out,values,count*sizeof(**out));return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
    CHECK(out!=NULL);*out=NULL;
    if(Guid(guid,&PathId)){
        if(h==TARGET)*out=(VOID*)TargetPath;
        else if(h==BRIDGE)*out=(VOID*)BridgePath;
        else if(h==INTEL)*out=(VOID*)IntelPath;
        else if(h==CHILD)*out=ChildPath;
        else if(h==GOP)*out=(VOID*)((Mode==GOP_WRONG_PATH||(Mode==GOP_AFTER&&Active)||(Mode==GOP_RELEASE&&ReleasePhase))?TargetPath:IntelPath);
        return *out?EFI_SUCCESS:EFI_UNSUPPORTED;
    }
    if(Guid(guid,&PciId)){
        if(h==TARGET)*out=&TargetPci;else if(h==BRIDGE)*out=&BridgePci;else if(h==INTEL)*out=&IntelPci;
        return *out?EFI_SUCCESS:EFI_UNSUPPORTED;
    }
    CHECK(Guid(guid,&GopId)&&h==GOP);*out=&Gop;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Protocols(EFI_HANDLE h,EFI_GUID ***out,UINTN *n) {
    CHECK(h==TARGET);*n=2;*out=malloc(2*sizeof(**out));CHECK(*out!=NULL);(*out)[0]=&PciId;(*out)[1]=&PathId;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Information(EFI_HANDLE h,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *n) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY values[3];UINTN count=0;CHECK(h==TARGET);
    memset(values,0,sizeof(values));
    values[count].AgentHandle=OTHER;values[count].Attributes=EFI_OPEN_PROTOCOL_GET_PROTOCOL;values[count++].OpenCount=1;
    if(Guid(guid,&PciId)&&Active){values[count].AgentHandle=IMAGE;values[count].ControllerHandle=TARGET;
        values[count].Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;values[count++].OpenCount=1;}
    if(Guid(guid,&PciId)&&(Mode==OWNER_BEFORE||(Mode==OWNER_AFTER&&Active)||(Mode==OWNER_RELEASE&&ReleasePhase))){
        values[count].AgentHandle=OTHER;values[count].ControllerHandle=TARGET;
        values[count].Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;values[count++].OpenCount=1;}
    *n=count;*out=malloc(count*sizeof(**out));CHECK(*out!=NULL);memcpy(*out,values,count*sizeof(**out));return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE h,EFI_GUID *guid,VOID **out,EFI_HANDLE agent,EFI_HANDLE controller,UINT32 attr) {
    CHECK(h==TARGET&&Guid(guid,&PciId)&&out&&agent==IMAGE&&controller==TARGET&&attr==EFI_OPEN_PROTOCOL_BY_DRIVER);
    CHECK(!Active);Opens++;if(Mode==OPEN_DENIED)return EFI_ACCESS_DENIED;
    Active=1;*out=Mode==OPEN_WRONG_POINTER?&BridgePci:&TargetPci;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_HANDLE h,EFI_GUID *guid,EFI_HANDLE agent,EFI_HANDLE controller) {
    CHECK(h==TARGET&&Guid(guid,&PciId)&&agent==IMAGE&&controller==TARGET&&Active);Closes++;
    if(Mode==CLOSE_FAIL)return EFI_DEVICE_ERROR;Active=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_PCI_IO_PROTOCOL *pci,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 o,UINTN n,VOID *out) {
    CHECK(w==EfiPciIoWidthUint32&&o==0&&out);
    if(pci==&IntelPci){CHECK(n==16);if(Mode==INTEL_READ_FAIL)return EFI_DEVICE_ERROR;memcpy(out,IntelCfg,64);}
    else{CHECK(pci==&BridgePci&&n==64);memcpy(out,BridgeCfg,256);
        if((Mode==BRIDGE_AFTER&&Active)||(Mode==BRIDGE_RELEASE&&ReleasePhase))((UINT8*)out)[0x40]^=1;}
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Attr(EFI_PCI_IO_PROTOCOL *pci,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 a,UINT64 *out) {
    CHECK(pci==&BridgePci&&op==EfiPciIoAttributeOperationGet&&a==0&&out);
    *out=Mode==BRIDGE_CACHE_OFF?0:EFI_PCI_IO_ATTRIBUTE_MEMORY;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *pci,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
    CHECK(pci==&IntelPci);*s=0;*b=0;*d=Mode==INTEL_WRONG_BDF?3:2;*f=0;return EFI_SUCCESS;
}
static void Setup(unsigned mode) {
    CHECK(!Stage2ReservationHeld());Mode=mode;Opens=Closes=Active=ReleasePhase=0;
    memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));memset(&TargetPci,0,sizeof(TargetPci));
    memset(&BridgePci,0,sizeof(BridgePci));memset(&IntelPci,0,sizeof(IntelPci));
    memset(&Gop,0,sizeof(Gop));memset(&GopMode,0,sizeof(GopMode));memset(&GopInfo,0,sizeof(GopInfo));
    memset(BridgeCfg,0,sizeof(BridgeCfg));memset(IntelCfg,0,sizeof(IntelCfg));
    Put(IntelCfg,0x46808086,4);IntelCfg[11]=3;
    if(mode==INTEL_WRONG_VENDOR)IntelCfg[0]^=1;if(mode==INTEL_WRONG_DEVICE)IntelCfg[2]^=1;if(mode==INTEL_WRONG_CLASS)IntelCfg[11]=2;
    BridgeCfg[14]=1;BridgeCfg[11]=6;BridgeCfg[10]=4;Put(BridgeCfg+4,mode==BRIDGE_MEM_OFF?0:2,2);
    BridgeCfg[0x19]=1;BridgeCfg[0x1a]=1;Put(BridgeCfg+0x20,0x8300,2);Put(BridgeCfg+0x22,mode==BRIDGE_WINDOW_BAD?0x82f0:0x83f0,2);
    memcpy(ChildPath,TargetPath,24);ChildPath[24]=1;ChildPath[25]=1;ChildPath[26]=6;ChildPath[27]=0;ChildPath[28]=0;ChildPath[29]=0;
    memcpy(ChildPath+30,TargetPath+24,4);
    Gop.Mode=&GopMode;GopMode.Info=mode==GOP_BAD_INFO?NULL:&GopInfo;GopMode.SizeOfInfo=sizeof(GopInfo);GopMode.FrameBufferBase=0x6000000000;GopMode.FrameBufferSize=0x100000;
    BridgePci.Pci.Read=Read;BridgePci.Attributes=Attr;IntelPci.Pci.Read=Read;IntelPci.GetLocation=Location;
    Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;Bs.ProtocolsPerHandle=Protocols;Bs.OpenProtocolInformation=Information;
    Bs.OpenProtocol=Open;Bs.CloseProtocol=Close;Bs.FreePool=Free;St.BootServices=&Bs;
}
int main(void) {
    unsigned mode;TEXT t;EFI_STATUS rc,released;
    for(mode=OK;mode<=GOP_RELEASE;mode++) {
        BOOLEAN successful=(BOOLEAN)(mode==OK||mode>=CLOSE_FAIL);
        BOOLEAN reserved=(BOOLEAN)(successful||(mode>=OPEN_WRONG_POINTER&&mode<=GOP_AFTER));
        Setup(mode);TextInit(&t,Output,sizeof(Output));
        rc=Stage2Reserve(&t,IMAGE,&St,TARGET,&TargetPci);CHECK(!t.failed);
        CHECK((!EFI_ERROR(rc))==successful);CHECK(Stage2ReservationHeld()==reserved);CHECK(Active==(unsigned)reserved);
        CHECK(Opens==((mode==OK||mode>=OPEN_DENIED)?1u:0u));
        if(mode==OK){CHECK(Stage2Reserve(&t,IMAGE,&St,TARGET,&TargetPci)==EFI_ALREADY_STARTED);CHECK(Opens==1);
            CHECK(Stage2Recheck(IMAGE,&St)==EFI_SUCCESS);
            Mode=GOP_AFTER;CHECK(EFI_ERROR(Stage2Recheck(IMAGE,&St)));
            Mode=OK;CHECK(Stage2Recheck(IMAGE,&St)==EFI_SUCCESS);}
        ReleasePhase=1;released=Stage2Release(&t,IMAGE,&St);CHECK(!t.failed);
        if(mode==CLOSE_FAIL){CHECK(EFI_ERROR(released)&&Stage2ReservationHeld()&&Active&&Closes==1);
            Mode=OK;CHECK(Stage2Release(&t,IMAGE,&St)==EFI_SUCCESS);CHECK(Closes==2);}
        else {
            BOOLEAN releaseError=(BOOLEAN)(mode==OWNER_AFTER||mode==BRIDGE_AFTER||mode==GOP_AFTER||mode>=BRIDGE_RELEASE);
            CHECK(EFI_ERROR(released)==releaseError);CHECK(Closes==(reserved?1u:0u));
        }
        CHECK(!Stage2ReservationHeld()&&!Active);
        CHECK(Stage2Recheck(IMAGE,&St)==EFI_ACCESS_DENIED);
        CHECK(Stage2Release(&t,IMAGE,&St)==EFI_SUCCESS);CHECK(!t.failed);
    }
    printf("admission: %u assertions, %u topology/ownership/failure scenarios passed\n",Checks,mode);return 0;
}
