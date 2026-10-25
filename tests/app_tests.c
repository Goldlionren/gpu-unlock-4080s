/* SPDX-License-Identifier: MIT
 * Host-only EfiMain integration. Real application control flow; platform,
 * admission, asset/hash and firmware-core operations are explicit mocks.
 * No firmware/GPU execution or claim of cryptographic/hardware validation.
 */
#ifndef GU_APP_HOST_TEST
#define GU_APP_HOST_TEST
#endif
#ifndef UNLOCK_BUILD_ID
#define UNLOCK_BUILD_ID "host-test"
#endif
#include "../src/port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#undef EPROTO
#undef EBADMSG
#undef EOVERFLOW
#undef EOPNOTSUPP
#undef ETIMEDOUT
#undef EALREADY
#include "../src/app.c"

static unsigned Checks,Mode,ConfigReads,MemWrites,Reserves,Releases,Restores,CoreCalls,CoreLoads;
static unsigned Starts,Loads,Unloads,KeyCalls,IoCloses,AssetsReleased,HashCalls,PromCopies,Stalls,Containments;
static unsigned WorkingClears,StopContainments;
static unsigned FinalReads;
static BOOLEAN Held,LogOpen;
static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rs;
static EFI_SYSTEM_TABLE St;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;
static EFI_PCI_IO_PROTOCOL Pci;
static EFI_LOADED_IMAGE_PROTOCOL Loaded,WinLoaded;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_FILE_PROTOCOL Root,WinFile;
static UINT32 Config[64],Initial[64];
static struct gpu_unlock_610_outcome Outcome;
static jmp_buf Handoff;
static VOID *Allocations[16];static UINTN AllocCount;
static CHAR8 LastConsole[512];
static CHAR8 LogCapture[16384];
static size_t LogLength;
static EFI_HANDLE Img=(EFI_HANDLE)(UINTN)100,Usb=(EFI_HANDLE)(UINTN)101,
    Gpu=(EFI_HANDLE)(UINTN)1,Internal=(EFI_HANDLE)(UINTN)201,
    Other=(EFI_HANDLE)(UINTN)202,Win=(EFI_HANDLE)(UINTN)300;
static UINT8 InternalPath[]={1,1,6,0,0,2,0x7f,0xff,4,0};
GU_PLATFORM Gu;GU_ASSET GuAssets[5];BOOLEAN GuScreenDetached;
enum {SUCCESS,SECURE_ON,LOG_FAIL,WATCHDOG_FAIL,TARGET_BAD,ASSET_FAIL,NO_WINDOWS,MULTI_WINDOWS,
    LOAD_FAIL,CANCEL,KEY_TIMEOUT,ADMISSION_FAIL,ADMISSION_PARTIAL,MEM_ENABLE_PARTIAL,MEM_IGNORED,
    PROFILE_FAIL,PROM_BAD,CORE_LOAD_FAIL,CORE_STARTED_FAIL,OUTCOME_BAD,SELECTOR_BAD,
    RESTORE_FAIL,RELEASE_FAIL,START_RETURNS,LOG_CLOSE_FAIL,MASTER_FAIL,UNSAFE_CORE,
    MEM_BME_UNEXPECTED,MEM_IO_BME_UNEXPECTED,PROM_CFG_DRIFT,LOG_CFG_DRIFT,LOAD_WRONG_VOLUME,LOAD_WRONG_TYPE,LOAD_NULL_OBJECT,
    POST_CORE_CFG_DRIFT,FINAL_CONFIG_READ_FAIL,RESTORE_PARTIAL_ERROR,
    RUN03_STATUS_CLEAR,STATUS_NEW_ERROR,STATUS_CONTROL_DRIFT,PREFIRMWARE_STATUS_CLEAR,STATUS_CLEAR_NO_FLR};
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"app FAIL line %d mode %u: %s\n",__LINE__,Mode,#x);exit(1);}}while(0)

static void Put(UINT8 *p,UINT64 value,UINTN bytes){for(UINTN i=0;i<bytes;i++)p[i]=(UINT8)(value>>(i*8));}
static EFI_STATUS EFIAPI ReadCfg(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *out){
    CHECK(self==&Pci&&width==EfiPciIoWidthUint32&&off==0&&count==64);ConfigReads++;
    if(Restores)FinalReads++;
    if(Mode==FINAL_CONFIG_READ_FAIL&&Restores){memset(out,0xa5,sizeof(Config));return EFI_DEVICE_ERROR;}
    memcpy(out,Config,sizeof(Config));return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI WriteCfg(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *in){
    CHECK(self==&Pci&&width==EfiPciIoWidthUint16&&off==4&&count==1&&Read16(in)==2);
    CHECK(Held&&LogOpen&&Reserves==1&&CoreCalls==0);MemWrites++;
    if(Mode!=MEM_IGNORED)Config[1]=(Config[1]&0xffff0000u)|(Mode==MEM_BME_UNEXPECTED?6:Mode==MEM_IO_BME_UNEXPECTED?7:2);
    return Mode==MEM_ENABLE_PARTIAL?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI UnusedMem(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 bar,UINT64 off,UINTN n,VOID *data){
    (void)self;(void)w;(void)bar;(void)off;(void)n;(void)data;CHECK(0);return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *self,UINTN *segment,UINTN *bus,UINTN *dev,UINTN *fun){
    CHECK(self==&Pci);*segment=0;*bus=1;*dev=*fun=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Attr(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 attr,UINT64 *out){
    CHECK(self==&Pci&&op==EfiPciIoAttributeOperationGet&&attr==0&&out);*out=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Bar(EFI_PCI_IO_PROTOCOL *self,UINT8 index,UINT64 *support,VOID **resource){
    CHECK(self==&Pci&&index==0&&support&&resource);UINT8 *r=calloc(48,1);CHECK(r!=NULL);
    r[0]=0x8a;Put(r+1,43,2);Put(r+6,32,8);Put(r+14,0x83000000,8);Put(r+22,0x83ffffff,8);
    Put(r+38,0x1000000,8);r[46]=0x79;*resource=r;*support=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI UnusedAlloc(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE a,EFI_MEMORY_TYPE t,UINTN n,VOID **out,UINT64 attr){
    (void)p;(void)a;(void)t;(void)n;(void)out;(void)attr;CHECK(0);return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI UnusedFree(EFI_PCI_IO_PROTOCOL *p,UINTN n,VOID *v){(void)p;(void)n;(void)v;CHECK(0);return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI UnusedMap(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION o,VOID *v,UINTN *n,EFI_PHYSICAL_ADDRESS *a,VOID **m){
    (void)p;(void)o;(void)v;(void)n;(void)a;(void)m;CHECK(0);return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI UnusedUnmap(EFI_PCI_IO_PROTOCOL *p,VOID *m){(void)p;(void)m;CHECK(0);return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI UnusedFlush(EFI_PCI_IO_PROTOCOL *p){(void)p;CHECK(0);return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI FreePool(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **handles){
    CHECK(type==ByProtocol&&key==NULL);
    if(BytesEqual(guid,&PciGuid,16)){
        *count=1;*handles=malloc(sizeof(EFI_HANDLE));CHECK(*handles);(*handles)[0]=Gpu;return EFI_SUCCESS;
    }
    CHECK(BytesEqual(guid,&FsGuid,16));*count=Mode==MULTI_WINDOWS?3:2;
    *handles=malloc(*count*sizeof(EFI_HANDLE));CHECK(*handles);(*handles)[0]=Usb;(*handles)[1]=Internal;
    if(*count==3)(*handles)[2]=Other;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out){
    if(h==Gpu&&BytesEqual(guid,&PciGuid,16)){CHECK(!Releases||Held);*out=&Pci;return EFI_SUCCESS;}
    if(h==Img&&BytesEqual(guid,&LoadedGuid,16)){*out=&Loaded;return EFI_SUCCESS;}
    if(h==Win&&BytesEqual(guid,&LoadedGuid,16)){*out=Mode==LOAD_NULL_OBJECT?NULL:&WinLoaded;return EFI_SUCCESS;}
    if((h==Internal||h==Other)&&BytesEqual(guid,&FsGuid,16)){*out=&Fs;return EFI_SUCCESS;}
    if(h==Internal&&BytesEqual(guid,&PathGuid,16)){*out=InternalPath;return EFI_SUCCESS;}
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,EFI_FILE_PROTOCOL **out){CHECK(self==&Fs);*out=&Root;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI OpenFile(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attrs){
    CHECK(self==&Root&&name[0]==L'\\'&&mode==EFI_FILE_MODE_READ&&attrs==0);
    if(Mode==NO_WINDOWS){*out=NULL;return EFI_NOT_FOUND;}*out=&WinFile;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseFile(EFI_FILE_PROTOCOL *self){CHECK(self==&Root||self==&WinFile);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Load(BOOLEAN policy,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *path,VOID *source,UINTN size,EFI_HANDLE *out){
    CHECK(!policy&&parent==Img&&path&&source==NULL&&size==0&&MemWrites==0);Loads++;
    if(Mode==LOAD_FAIL)return EFI_SECURITY_VIOLATION;*out=Win;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE h){CHECK(h==Win&&Starts==0);Unloads++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Start(EFI_HANDLE h,UINTN *size,CHAR16 **data){
    CHECK(h==Win&&size&&data&&CoreCalls==1&&Restores==1&&Releases==1&&!Held);
    if(Mode==RUN03_STATUS_CLEAR){
        CHECK(Initial[32]==0x00092930&&Config[32]==0x00002930);
        for(UINTN i=0;i<64;i++)if(i!=32)CHECK(Config[i]==Initial[i]);
    } else CHECK(!memcmp(Config,Initial,sizeof(Config)));
    CHECK(AssetsReleased==1&&IoCloses==1&&!Gu.armed&&!Gu.retained);
    Starts++;if(Mode==START_RETURNS)return EFI_DEVICE_ERROR;longjmp(Handoff,1);
}
static EFI_STATUS EFIAPI Variable(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,UINTN *size,VOID *value){
    CHECK(name[0]==L'S'&&BytesEqual(guid,&GlobalGuid,16)&&attrs==NULL&&*size==1);
    *(UINT8*)value=Mode==SECURE_ON?1:0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Watchdog(UINTN t,UINT64 code,UINTN n,CHAR16 *data){
    CHECK(t==0&&code==0&&n==0&&data==NULL&&MemWrites==0);return Mode==WATCHDOG_FAIL?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Stall(UINTN us){CHECK(us==100000||us==8000000);Stalls++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,EFI_INPUT_KEY *key){
    CHECK(self==&Input);if(KeyCalls++==0||Mode==KEY_TIMEOUT)return EFI_NOT_READY;
    key->UnicodeChar=Mode==CANCEL?0:L'U';key->ScanCode=Mode==CANCEL?0x17:0;return EFI_SUCCESS;
}

VOID GuInit(EFI_BOOT_SERVICES *bs,EFI_PCI_IO_PROTOCOL *pci){memset(&Gu,0,sizeof(Gu));Gu.bs=bs;Gu.pci=pci;}
BOOLEAN GuTargetConfig(const UINT32 *cfg,UINTN s,UINTN b,UINTN d,UINTN f){CHECK(s==0&&b==1&&d==0&&f==0);return cfg[0]==GU_BOARD_ID&&Mode!=TARGET_BAD;}
BOOLEAN GuCheckConfig(const UINT32 cfg[64],UINTN s,UINTN b,UINTN d,UINTN f,GU_CAPS *caps){
    CHECK((cfg[1]&0xffff)==2&&s==0&&b==1&&d==0&&f==0);memset(caps,0,sizeof(*caps));caps->pm=0x60;caps->pcie=0x78;caps->pcie_version=2;return TRUE;
}
EFI_STATUS GuIoOpen(EFI_HANDLE image,EFI_SYSTEM_TABLE *st){CHECK(image==Img&&st==&St);LogOpen=Mode!=LOG_FAIL;return LogOpen?EFI_SUCCESS:EFI_WRITE_PROTECTED;}
EFI_STATUS GuIoClose(VOID){IoCloses++;LogOpen=FALSE;return Mode==LOG_CLOSE_FAIL?EFI_DEVICE_ERROR:EFI_SUCCESS;}
EFI_STATUS GuLoadAssets(VOID){return Mode==ASSET_FAIL?EFI_CRC_ERROR:EFI_SUCCESS;}
VOID GuReleaseAssets(VOID){AssetsReleased++;}
BOOLEAN GuLogReady(VOID){return LogOpen;}
VOID GuLog(const char *format,...){
    va_list args;int written;CHECK(format!=NULL);
    if(strstr(format,"entry ")&&(Mode==RESTORE_FAIL||Mode==RESTORE_PARTIAL_ERROR||Mode==FINAL_CONFIG_READ_FAIL||
       Mode==POST_CORE_CFG_DRIFT||Mode==MEM_BME_UNEXPECTED||Mode==MEM_IO_BME_UNEXPECTED||Mode==PROM_CFG_DRIFT||Mode==LOG_CFG_DRIFT||
       Mode==STATUS_NEW_ERROR||Mode==STATUS_CONTROL_DRIFT||Mode==PREFIRMWARE_STATUS_CLEAR||Mode==STATUS_CLEAR_NO_FLR))
        CHECK(StopContainments==1);
    va_start(args,format);written=vsnprintf(LogCapture+LogLength,sizeof(LogCapture)-LogLength,format,args);va_end(args);
    CHECK(written>=0&&(size_t)written<sizeof(LogCapture)-LogLength);LogLength+=(size_t)written;
    if(Mode==LOG_CFG_DRIFT&&strstr(format,"PROM double-read"))Config[20]^=1;
}
VOID GuLogText(const char *text){CHECK(text!=NULL);}
VOID GuConsole(const char *text){CHECK(text!=NULL);snprintf(LastConsole,sizeof(LastConsole),"%s",text);}
VOID *GuAlloc(size_t bytes){VOID *p=malloc(bytes);CHECK(p&&AllocCount<ARRAY_SIZE(Allocations));Allocations[AllocCount++]=p;return p;}
VOID GuFree(VOID *p){if(!p)return;for(UINTN i=0;i<AllocCount;i++)if(Allocations[i]==p){free(p);Allocations[i]=Allocations[--AllocCount];return;}CHECK(0);}
VOID GuStall(UINTN us){CHECK(us==100000);Stalls++;}
VOID GuCopyFromIo(VOID *dst,const VOID *src,size_t n){
    CHECK(dst&&(UINTN)src==0x83300000&&n==1048576&&MemWrites==1&&Held);PromCopies++;memset(dst,0x5a,n);
    if(Mode==PROM_CFG_DRIFT&&PromCopies==1)Config[20]^=1;
    if(Mode==PREFIRMWARE_STATUS_CLEAR&&PromCopies==1)Config[32]=0x00002930;
}
BOOLEAN GuHashMatches(const VOID *data,UINTN n,const char *hash){CHECK(data&&(n==1048576||n==GU_PROM_VIEW_BYTES)&&strlen(hash)==64);HashCalls++;return Mode!=PROM_BAD;}
UINT32 GuRead32(UINT32 offset){
    static const UINT32 offsets[]={0,0x1fa828,0x1fa824,0x118128,0x118234,0x110100,0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0};
    static const UINT32 values[]={0x193000a1,0,0x1ffffe00,0x8b8f,0x3ff,0x10,0x4cb8f,0x4cb8f,0xffffff0f,0xffffff8f,0x233,0x410080,0x64113035,2,0x3ff8,1,0x45cb8f,0x100000,0x3cf7ff0,1,1};
    CHECK(Held&&(Config[1]&0xffff)==2);
    if(CoreCalls){if(offset==0x82381c)return Mode==SELECTOR_BAD?0x64113035:0x88888888;if(offset==0x823820)return 8;if(offset==0x1fa828)return 0;if(offset==0x110100)return 0x10;CHECK(0);}
    for(UINTN i=0;i<ARRAY_SIZE(offsets);i++)if(offsets[i]==offset)return Mode==PROFILE_FAIL&&i==0?0:values[i];
    CHECK(0);return 0xffffffff;
}
EFI_STATUS Stage2Reserve(TEXT *t,EFI_HANDLE image,EFI_SYSTEM_TABLE *st,EFI_HANDLE target,EFI_PCI_IO_PROTOCOL *pci){
    CHECK(image==Img&&st==&St&&target==Gpu&&pci==&Pci&&MemWrites==0);Reserves++;
    Held=Mode!=ADMISSION_FAIL;TextPut(t,"{\"mock\":true}");return Mode==ADMISSION_FAIL||Mode==ADMISSION_PARTIAL?EFI_ACCESS_DENIED:EFI_SUCCESS;
}
BOOLEAN Stage2ReservationHeld(VOID){return Held;}
EFI_STATUS Stage2Recheck(EFI_HANDLE image,EFI_SYSTEM_TABLE *st){CHECK(image==Img&&st==&St&&Held);return EFI_SUCCESS;}
EFI_STATUS Stage2Release(TEXT *t,EFI_HANDLE image,EFI_SYSTEM_TABLE *st){
    CHECK(image==Img&&st==&St&&Held&&(Config[1]&0xffff)==0);Releases++;TextPut(t,"{\"mock\":true}");
    if(Mode==RELEASE_FAIL)return EFI_DEVICE_ERROR;Held=FALSE;return EFI_SUCCESS;
}
EFI_STATUS GuRestoreEntryCommandZero(VOID){
    CHECK(Held&&MemWrites==1);Restores++;
    if(Mode==RESTORE_FAIL||(Config[1]&4))return EFI_DEVICE_ERROR;
    Config[1]&=0xffff0000u;
    if(Mode==RESTORE_PARTIAL_ERROR){Gu.fault=Gu.retained=TRUE;return EFI_DEVICE_ERROR;}
    return EFI_SUCCESS;
}
EFI_STATUS GuClearMaster(VOID){
    CHECK(Held&&Gu.pci==&Pci&&(Config[1]&0xffff)==2);Containments++;WorkingClears++;
    if(Mode==MASTER_FAIL)return EFI_DEVICE_ERROR;return EFI_SUCCESS;
}
EFI_STATUS GuContainMasterForStop(VOID){
    CHECK(Held&&Gu.pci==&Pci);Containments++;StopContainments++;
    Config[1]&=~4u;return EFI_SUCCESS;
}
BOOLEAN GuSafeToReturn(VOID){return Mode!=UNSAFE_CORE;}
int gpu_unlock_610_load_shared(struct pci_dev *p,struct gpu_unlock_610_shared *shared){CHECK(p&&shared&&Gu.armed&&Held);CoreLoads++;return Mode==CORE_LOAD_FAIL?-5:0;}
int gpu_unlock_610_unlock(struct pci_dev *p,const struct gpu_unlock_610_shared *shared){
    CHECK(p&&shared&&Gu.armed&&Held&&MemWrites==1);CoreCalls++;Gu.started=TRUE;Gu.quiesced=TRUE;
    Gu.flrs=Mode==STATUS_CLEAR_NO_FLR?0:1;
    Outcome.firmware_started=Outcome.required_plm_verified=Outcome.all_plm_verified=Outcome.selectors_before_flr=Outcome.selectors_after_flr=Outcome.recovered=1;
    if(Mode==POST_CORE_CFG_DRIFT)Config[20]^=0x4000;
    if(Mode==RUN03_STATUS_CLEAR||Mode==STATUS_CLEAR_NO_FLR)Config[32]=0x00002930;
    if(Mode==STATUS_NEW_ERROR)Config[32]=0x00022930;
    if(Mode==STATUS_CONTROL_DRIFT)Config[32]=0x00002931;
    if(Mode==OUTCOME_BAD)Outcome.selectors_after_flr=0;return Mode==CORE_STARTED_FAIL?-5:0;
}
const struct gpu_unlock_610_outcome *gpu_unlock_610_get_outcome(VOID){return &Outcome;}
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared){CHECK(shared&&CoreLoads==1);}

static void Setup(unsigned mode){
    while(AllocCount)free(Allocations[--AllocCount]);
    memset(&Bs,0,sizeof(Bs));memset(&Rs,0,sizeof(Rs));memset(&St,0,sizeof(St));memset(&Pci,0,sizeof(Pci));
    memset(Config,0,sizeof(Config));memset(&Outcome,0,sizeof(Outcome));memset(LastConsole,0,sizeof(LastConsole));
    memset(LogCapture,0,sizeof(LogCapture));LogLength=0;WorkingClears=StopContainments=FinalReads=0;
    Config[0]=GU_BOARD_ID;Config[1]=0x00100000;Config[2]=0x030000a1;Config[4]=0x83000000;
    Config[5]=0xc;Config[6]=0x60;Config[7]=0xc;Config[8]=0x64;Config[9]=0x3001;
    Config[11]=GU_BOARD_SUBSYSTEM;Config[12]=0x84000000;Config[32]=0x00092930;memcpy(Initial,Config,sizeof(Config));
    Pci.Pci.Read=ReadCfg;Pci.Pci.Write=WriteCfg;Pci.Mem.Read=UnusedMem;Pci.Mem.Write=UnusedMem;
    Pci.GetLocation=Location;Pci.Attributes=Attr;Pci.GetBarAttributes=Bar;Pci.AllocateBuffer=UnusedAlloc;
    Pci.FreeBuffer=UnusedFree;Pci.Map=UnusedMap;Pci.Unmap=UnusedUnmap;Pci.Flush=UnusedFlush;
    Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;Bs.FreePool=FreePool;Bs.LoadImage=Load;
    Bs.StartImage=Start;Bs.UnloadImage=Unload;Bs.Stall=Stall;Bs.SetWatchdogTimer=Watchdog;
    Rs.GetVariable=Variable;Input.ReadKeyStroke=Key;St.BootServices=&Bs;St.RuntimeServices=&Rs;St.ConIn=&Input;
    Loaded.DeviceHandle=Usb;WinLoaded.DeviceHandle=mode==LOAD_WRONG_VOLUME?Usb:Internal;
    WinLoaded.ImageCodeType=mode==LOAD_WRONG_TYPE?EfiBootServicesCode:EfiLoaderCode;
    Fs.OpenVolume=OpenVolume;Root.Open=OpenFile;Root.Close=CloseFile;WinFile.Close=CloseFile;
    Mode=mode;ConfigReads=MemWrites=Reserves=Releases=Restores=CoreCalls=CoreLoads=0;
    Starts=Loads=Unloads=KeyCalls=IoCloses=AssetsReleased=HashCalls=PromCopies=Stalls=Containments=0;Held=LogOpen=FALSE;
}
static void FinalPolicyTests(void){
    UINT32 entry[64],final[64],unchanged[64];BOOLEAN accepted;
    for(UINTN i=0;i<64;i++)entry[i]=0x10203040u+(UINT32)i;
    entry[32]=0x00092930;memcpy(final,entry,sizeof(final));memcpy(unchanged,entry,sizeof(entry));
    accepted=TRUE;CHECK(FinalEntryMatches(final,entry,FALSE,0,&accepted)&&!accepted);
    final[32]=0x00002930;accepted=FALSE;CHECK(FinalEntryMatches(final,entry,TRUE,0x78,&accepted)&&accepted);
    accepted=TRUE;CHECK(!FinalEntryMatches(final,entry,FALSE,0x78,&accepted)&&!accepted);
    const UINT16 wrongCaps[]={0,0x74,0x7c,0xfc};
    for(UINTN i=0;i<ARRAY_SIZE(wrongCaps);i++){
        accepted=TRUE;CHECK(!FinalEntryMatches(final,entry,TRUE,wrongCaps[i],&accepted)&&!accepted);
    }
    /* Every single-bit change anywhere in the otherwise accepted final image
     * must fail, including controls, newly set status and partially clear status. */
    for(UINTN word=0;word<64;word++)for(unsigned bit=0;bit<32;bit++){
        final[word]^=1u<<bit;accepted=TRUE;
        CHECK(!FinalEntryMatches(final,entry,TRUE,0x78,&accepted)&&!accepted);
        final[word]^=1u<<bit;
    }
    /* The exact entry value is also required; no broad status masks. */
    for(unsigned bit=0;bit<32;bit++){
        entry[32]^=1u<<bit;accepted=TRUE;
        CHECK(!FinalEntryMatches(final,entry,TRUE,0x78,&accepted)&&!accepted);
        entry[32]^=1u<<bit;
    }
    CHECK(!memcmp(entry,unchanged,sizeof(entry))&&final[32]==0x00002930);
    for(UINTN i=0;i<64;i++)if(i!=32)CHECK(final[i]==unchanged[i]);
}
int main(void){
    unsigned mode;
    for(mode=SUCCESS;mode<=STATUS_CLEAR_NO_FLR;mode++){
        EFI_STATUS result=EFI_SUCCESS;Setup(mode);
        if(setjmp(Handoff)==0){result=EfiMain(Img,&St);CHECK(mode!=SUCCESS&&mode!=RUN03_STATUS_CLEAR);}
        else CHECK((mode==SUCCESS||mode==RUN03_STATUS_CLEAR)&&Starts==1&&strcmp(Phase,"windows-start")==0&&!Gu.retained);
        if(mode==SUCCESS||mode==RUN03_STATUS_CLEAR){
            CHECK(Restores==1&&Releases==1&&CoreCalls==1&&HashCalls==2&&PromCopies==2&&WorkingClears==1&&StopContainments==0);
            if(mode==SUCCESS)CHECK(strstr(LogCapture,"command_status=0 config_read_status=0 compared=1 differences=0")!=NULL);
            else {
                CHECK(strstr(LogCapture,"entry config delta: offset=0x80 entry=0x92930 final=0x2930 xor=0x90000")!=NULL);
                CHECK(strstr(LogCapture,"verified_flr=1 accepted_ced_urd_clear=1 configuration_accepted=1")!=NULL);
                CHECK(FinalRecoveryVerified&&StatusClearAccepted&&FinalConfigAccepted&&FinalReads==1);
            }
            continue;
        }
        CHECK(EFI_ERROR(result));
        if(mode<=ADMISSION_FAIL){CHECK(MemWrites==0&&CoreCalls==0&&Starts==0&&Restores==0);}
        if(mode==ADMISSION_PARTIAL)CHECK(MemWrites==0&&Releases==1&&!Held&&CoreCalls==0);
        if(mode==MEM_ENABLE_PARTIAL||mode==MEM_IGNORED||mode==PROFILE_FAIL||mode==PROM_BAD||mode==CORE_LOAD_FAIL)
            CHECK(Restores==1&&Releases==1&&!Held&&CoreCalls==0&&!memcmp(Config,Initial,sizeof(Config)));
        if(mode==CORE_STARTED_FAIL||mode==OUTCOME_BAD||mode==SELECTOR_BAD||mode==MASTER_FAIL||mode==UNSAFE_CORE)
            CHECK(result==EFI_ABORTED&&Gu.retained&&Starts==0&&Restores==0&&Releases==0&&Held);
        if(mode==RESTORE_FAIL){
            CHECK(result==EFI_ABORTED&&Gu.retained&&Starts==0&&Restores==1&&Releases==0&&Held);
            CHECK(WorkingClears==1&&StopContainments==1);
            CHECK(strstr(LogCapture,"command_status=0x8000000000000007 config_read_status=0x8000000000000013 compared=0 differences=0")!=NULL);
            CHECK(strstr(LogCapture,"entry config delta:")==NULL&&FinalReads==0);
        }
        if(mode==RELEASE_FAIL)CHECK(result==EFI_ABORTED&&Gu.retained&&Starts==0&&Restores==1&&Releases==1&&Held);
        if(mode==START_RETURNS)CHECK(result==EFI_ABORTED&&Gu.retained&&Starts==1&&Restores==1&&Releases==1&&!Held&&Containments==1&&WorkingClears==1&&StopContainments==0);
        if(mode==LOG_CLOSE_FAIL)CHECK(result==EFI_ABORTED&&Gu.retained&&Starts==0&&Restores==1&&Releases==1&&!Held&&Containments==1&&WorkingClears==1&&StopContainments==0);
        if(mode==MULTI_WINDOWS||mode==NO_WINDOWS)CHECK(Loads==0&&Reserves==0);
        if(mode==KEY_TIMEOUT)CHECK(Stalls==301);
        if(mode==MEM_BME_UNEXPECTED||mode==MEM_IO_BME_UNEXPECTED)
            CHECK(result==EFI_ABORTED&&Gu.retained&&CoreCalls==0&&Starts==0&&Restores==1&&Releases==0&&Held&&Containments==1&&!(Config[1]&4));
        if(mode==PROM_CFG_DRIFT||mode==LOG_CFG_DRIFT)
            CHECK(result==EFI_ABORTED&&Gu.retained&&CoreCalls==0&&Starts==0&&Restores==1&&Releases==0&&Held);
        if(mode==LOAD_WRONG_VOLUME||mode==LOAD_WRONG_TYPE||mode==LOAD_NULL_OBJECT)
            CHECK(MemWrites==0&&Reserves==0&&Loads==1&&Unloads==1&&CoreCalls==0);
        if(mode==POST_CORE_CFG_DRIFT||mode==FINAL_CONFIG_READ_FAIL||mode==RESTORE_PARTIAL_ERROR){
            CHECK(result==EFI_ABORTED&&Gu.retained&&Starts==0&&Restores==1&&Releases==0&&Held&&CoreCalls==1);
            CHECK(WorkingClears==1&&StopContainments==1&&(Config[1]&0xffff)==0);
            CHECK(strstr(LogCapture,"STOP phase=entry-restore containment=0")!=NULL);
            CHECK(strstr(LogCapture,"FINAL status=")==NULL);
        }
        if(mode==POST_CORE_CFG_DRIFT){
            CHECK(strstr(LogCapture,"entry config delta: offset=0x50 entry=0 final=0x4000 xor=0x4000")!=NULL);
            CHECK(strstr(LogCapture,"command_status=0 config_read_status=0 compared=1 differences=1 fault=0 retained=1")!=NULL);
            CHECK(strstr(LogCapture,"fault_before_containment=0 retained_before_containment=0")!=NULL&&!Gu.fault);
        }
        if(mode==FINAL_CONFIG_READ_FAIL){
            CHECK(strstr(LogCapture,"command_status=0 config_read_status=0x8000000000000007 compared=0 differences=0")!=NULL);
            CHECK(strstr(LogCapture,"entry config delta:")==NULL&&!Gu.fault&&FinalReads==1);
        }
        if(mode==RESTORE_PARTIAL_ERROR){
            CHECK(strstr(LogCapture,"command_status=0x8000000000000007 config_read_status=0x8000000000000013 compared=0 differences=0 fault=1 retained=1")!=NULL);
            CHECK(strstr(LogCapture,"fault_before_containment=1 retained_before_containment=1")!=NULL&&Gu.fault);
            CHECK(strstr(LogCapture,"entry config delta:")==NULL&&FinalReads==0);
        }
        if(mode==STATUS_NEW_ERROR||mode==STATUS_CONTROL_DRIFT||mode==PREFIRMWARE_STATUS_CLEAR||mode==STATUS_CLEAR_NO_FLR){
            CHECK(result==EFI_ABORTED&&Gu.retained&&Starts==0&&Restores==1&&Releases==0&&Held&&StopContainments==1);
            CHECK(!StatusClearAccepted&&!FinalConfigAccepted);
            CHECK(strstr(LogCapture,"accepted_ced_urd_clear=0 configuration_accepted=0")!=NULL);
            CHECK(strstr(LogCapture,"FINAL status=")==NULL);
            if(mode==PREFIRMWARE_STATUS_CLEAR)CHECK(CoreCalls==0&&CoreLoads==0&&!FinalRecoveryVerified&&Gu.flrs==0);
            else CHECK(CoreCalls==1&&FinalReads==1&&WorkingClears==1);
            if(mode==STATUS_CLEAR_NO_FLR)CHECK(!FinalRecoveryVerified&&Gu.flrs==0);
        }
    }
    FinalPolicyTests();
    Setup(SUCCESS);CHECK(EfiMain(Img,NULL)==EFI_INVALID_PARAMETER);
    while(AllocCount)free(Allocations[--AllocCount]);
    printf("app: %u assertions, %u application integration scenarios passed; non-app layers mocked, no hardware execution\n",Checks,mode);
    return 0;
}
