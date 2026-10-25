/* SPDX-License-Identifier: MIT - host-only PCI/DMA fault mocks, no hardware. */
#include "../src/port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Scenario,Writes,Reads,Unmaps,Frees,Allocs,Maps,AttrCalls,FlrWrites;
static EFI_BOOT_SERVICES Bs;static EFI_PCI_IO_PROTOCOL Pci;static UINT8 Config[256],Initial[256];
static UINT64 Cached,Stalled;static VOID *DmaCpu;static UINTN LastPages;
static UINT32 WriteOffsets[32];static UINT16 WriteValues[32];
enum {OK,WRONG_ID,INITIAL_MASTER,CACHE_NONZERO,NO_MASTER_SUPPORT,ENABLE_FAIL_PARTIAL,ENABLE_IGNORED,
  ENABLE_READ_ERROR,CACHE_CHANGED,CLEAR_FAIL,CLEAR_IGNORED,CLEAR_CACHE_ERROR,ENTRY_FAIL,
  ALLOC_FAIL,ALLOC_PARTIAL,MAP_FAIL,MAP_PARTIAL,MAP_SHORT,MAP_HIGH,MAP_CROSS,MAP_UNALIGNED,
  UNMAP_FAIL,FREE_FAIL,FLR_FAIL,FLR_PENDING,FLR_ID_GONE,RESTORE_BAR_IGNORED,RESTORE_ROM_IGNORED};
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"port fail line %d mode %u: %s\n",__LINE__,Scenario,#x);exit(1);}}while(0)
static UINT16 U16(const VOID *p){UINT16 v;memcpy(&v,p,2);return v;}
static void Put(UINTN off,UINT64 v,UINTN n){UINTN i;for(i=0;i<n;i++)Config[off+i]=(UINT8)(v>>(8*i));}
VOID GuLog(const char *format, ...){(void)format;}
VOID GuLogText(const char *text){(void)text;}
static EFI_STATUS EFIAPI Pool(EFI_MEMORY_TYPE type,UINTN n,VOID **out){CHECK(type==EfiLoaderData);*out=malloc(n);return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;}
static EFI_STATUS EFIAPI PoolFree(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Stall(UINTN us){Stalled+=us;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,VOID *out){
  CHECK(p==&Pci&&n==1&&(UINT32)w<=EfiPciIoWidthUint32&&off+(1u<<w)<=256);Reads++;
  if(FlrWrites)CHECK(Stalled>=100000);
  if(Scenario==ENABLE_READ_ERROR&&Writes==1&&off==4)return EFI_DEVICE_ERROR;
  memcpy(out,Config+off,(UINTN)1<<w);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,VOID *in){
  UINT32 v=0;CHECK(p==&Pci&&n==1&&(UINT32)w<=EfiPciIoWidthUint32&&off+(1u<<w)<=256);
  memcpy(&v,in,(UINTN)1<<w);CHECK(Writes<32);WriteOffsets[Writes]=off;WriteValues[Writes]=(UINT16)v;Writes++;
  if(off==4) {
    CHECK(w==EfiPciIoWidthUint16);CHECK(v==0||v==2||v==6);
    if(v==6&&Scenario==ENABLE_IGNORED)return EFI_SUCCESS;
    if(v==2&&Scenario==CLEAR_IGNORED)return EFI_SUCCESS;
    if(v==0&&Scenario==ENTRY_FAIL)return EFI_DEVICE_ERROR;
  }
  if(off>=0x10&&off<=0x24&&Scenario==RESTORE_BAR_IGNORED)return EFI_SUCCESS;
  if(off==0x30&&Scenario==RESTORE_ROM_IGNORED)return EFI_SUCCESS;
  memcpy(Config+off,in,(UINTN)1<<w);
  if(off==4&&v==6&&Scenario==ENABLE_FAIL_PARTIAL)return EFI_DEVICE_ERROR;
  if(off==4&&v==2&&Scenario==CLEAR_FAIL)return EFI_DEVICE_ERROR;
  if(off==0x80&&(v&0x8000)) {
    FlrWrites++;Stalled=0;Put(4,0,2);memset(Config+0x10,0,24);Put(0x30,0,4);
    Put(0x80,0,2);Put(0xa0,0,2);Put(0x0c,0,2);Put(0x3c,0,1);
    if(Scenario==FLR_ID_GONE)Put(0,0xffffffffu,4);
    if(Scenario==FLR_FAIL)return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 a,UINT64 *out){
  CHECK(p==&Pci&&a==0&&out);CHECK(op==EfiPciIoAttributeOperationGet||op==EfiPciIoAttributeOperationSupported);AttrCalls++;
  if(Scenario==CLEAR_CACHE_ERROR&&AttrCalls>=4)return EFI_DEVICE_ERROR;
  *out=op==EfiPciIoAttributeOperationSupported?(Scenario==NO_MASTER_SUPPORT?0:EFI_PCI_IO_ATTRIBUTE_BUS_MASTER):Cached;
  if(op==EfiPciIoAttributeOperationGet&&Writes&&Scenario==CACHE_CHANGED)*out=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Allocate(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE type,EFI_MEMORY_TYPE mt,UINTN pages,VOID **out,UINT64 attr){
  CHECK(p==&Pci&&type==AllocateAnyPages&&mt==EfiBootServicesData&&attr==0);Allocs++;
  if(Scenario==ALLOC_FAIL)return EFI_OUT_OF_RESOURCES;
  DmaCpu=malloc(pages*4096);CHECK(DmaCpu!=NULL);LastPages=pages;*out=DmaCpu;
  return Scenario==ALLOC_PARTIAL?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Map(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION op,VOID *cpu,UINTN *bytes,EFI_PHYSICAL_ADDRESS *addr,VOID **mapping){
  CHECK(p==&Pci&&op==EfiPciIoOperationBusMasterCommonBuffer&&cpu==DmaCpu&&*bytes==LastPages*4096);Maps++;
  if(Scenario==MAP_FAIL)return EFI_DEVICE_ERROR;
  *mapping=(VOID *)(UINTN)0x1234;*addr=0x100000;
  if(Scenario==MAP_PARTIAL)return EFI_DEVICE_ERROR;
  if(Scenario==MAP_SHORT)*bytes-=4096;if(Scenario==MAP_HIGH)*addr=0x100000000ULL;
  if(Scenario==MAP_CROSS)*addr=0xfffff000ULL;if(Scenario==MAP_UNALIGNED)*addr=0x100001;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap(EFI_PCI_IO_PROTOCOL *p,VOID *mapping){CHECK(p==&Pci&&mapping==(VOID *)(UINTN)0x1234);Unmaps++;return Scenario==UNMAP_FAIL?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI FreeBuffer(EFI_PCI_IO_PROTOCOL *p,UINTN pages,VOID *cpu){CHECK(p==&Pci&&pages==LastPages&&cpu==DmaCpu);Frees++;if(Scenario==FREE_FAIL)return EFI_DEVICE_ERROR;free(cpu);DmaCpu=NULL;return EFI_SUCCESS;}
static void Setup(unsigned mode){
  static const char hex[]="de10022700001000a100000310008000000000830c000000600000000c000000640000000130000000000000da190247000000846000000000000000ff010000da1902470000000000000000000000000000000001000000ced623000000000001680348080000000578800000000000000000000000000010b41200e18d2c113029090004794500420104110000000000000000000000000000000013080700000400001e00800104001e0000000000000000000900140100000000000000000000000000000000110005000000b9000000ba000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
  UINTN i;unsigned v;GU_DMA *entry=Gu.allocations;
  /* Release only host mock memory left by intentionally retained scenarios. */
  while(entry){GU_DMA *next=entry->next;free(entry);entry=next;}if(DmaCpu)free(DmaCpu);DmaCpu=NULL;
  Scenario=mode;Writes=Reads=Unmaps=Frees=Allocs=Maps=AttrCalls=FlrWrites=0;Cached=Stalled=0;
  memset(&Bs,0,sizeof(Bs));memset(&Pci,0,sizeof(Pci));
  for(i=0;i<256;i++){CHECK(sscanf(hex+2*i,"%2x",&v)==1);Config[i]=(UINT8)v;}
  Put(4,2,2);memcpy(Initial,Config,256);
  if(mode==WRONG_ID)Config[2]^=1;if(mode==INITIAL_MASTER)Put(4,6,2);if(mode==CACHE_NONZERO)Cached=EFI_PCI_IO_ATTRIBUTE_MEMORY;
  if(mode==FLR_PENDING)Config[0x82]|=0x20;
  Bs.AllocatePool=Pool;Bs.FreePool=PoolFree;Bs.Stall=Stall;
  Pci.Pci.Read=Read;Pci.Pci.Write=Write;Pci.Attributes=Attributes;
  Pci.AllocateBuffer=Allocate;Pci.FreeBuffer=FreeBuffer;Pci.Map=Map;Pci.Unmap=Unmap;
  GuInit(&Bs,&Pci);Gu.owned=Gu.armed=Gu.display_ok=TRUE;Gu.pcie_cap=0x78;Gu.pcie_version=2;
}
static void TestMaster(void){unsigned mode;EFI_STATUS s;
  for(mode=OK;mode<=ENTRY_FAIL;mode++) {
    Setup(mode);s=GuEnableMaster();
    if(mode==WRONG_ID||mode==INITIAL_MASTER||mode==CACHE_NONZERO||mode==NO_MASTER_SUPPORT){CHECK(EFI_ERROR(s)&&Writes==0);continue;}
    if(mode==ENABLE_FAIL_PARTIAL||mode==ENABLE_IGNORED||mode==ENABLE_READ_ERROR||mode==CACHE_CHANGED)CHECK(EFI_ERROR(s)&&Gu.fault&&Gu.master_attributes_pending);
    else CHECK(s==EFI_SUCCESS&&U16(Config+4)==6);
    if(mode==ENABLE_READ_ERROR)Scenario=OK;
    s=GuClearMaster();
    if(mode==CACHE_CHANGED||mode==CLEAR_FAIL||mode==CLEAR_IGNORED||mode==CLEAR_CACHE_ERROR){CHECK(EFI_ERROR(s)&&Gu.retained);continue;}
    CHECK(s==EFI_SUCCESS&&U16(Config+4)==2&&!Gu.master_attributes_pending&&Cached==0);
    s=GuRestoreEntryCommandZero();
    if(mode==ENTRY_FAIL) {
      CHECK(EFI_ERROR(s)&&Gu.retained);
      CHECK(!strcmp(Gu.entry_restore_step,"command-write-zero")&&Gu.entry_restore_api_status==EFI_DEVICE_ERROR);
      CHECK(Gu.entry_restore_before==2&&Gu.entry_restore_after==0xffff);
    } else {
      CHECK(s==EFI_SUCCESS&&U16(Config+4)==0);
      CHECK(!strcmp(Gu.entry_restore_step,"complete")&&Gu.entry_restore_api_status==EFI_SUCCESS);
      CHECK(Gu.entry_restore_before==2&&Gu.entry_restore_after==0&&Gu.entry_restore_attributes==0);
    }
  }
  Setup(OK);Gu.started=TRUE;CHECK(EFI_ERROR(GuRestoreEntryCommandZero())&&Writes==0);
  CHECK(!strcmp(Gu.entry_restore_step,"precondition"));
  Gu.quiesced=TRUE;Gu.fault=TRUE;CHECK(GuRestoreEntryCommandZero()==EFI_SUCCESS&&U16(Config+4)==0);
}
static void TestDma(void){unsigned mode;VOID *cpu;UINT64 dma;EFI_STATUS s;
  for(mode=ALLOC_FAIL;mode<=FREE_FAIL;mode++) {
    Setup(mode);s=GuDmaAllocate(8192,&cpu,&dma);
    if(mode==UNMAP_FAIL||mode==FREE_FAIL){CHECK(s==EFI_SUCCESS&&Gu.dma_count==1);s=GuDmaFree(cpu,dma);CHECK(EFI_ERROR(s)&&Gu.retained&&Gu.allocations);continue;}
    CHECK(EFI_ERROR(s)&&cpu==NULL&&dma==0);
    if(mode==ALLOC_PARTIAL||mode==MAP_PARTIAL)CHECK(Gu.retained&&Gu.allocations&&Frees==0&&Unmaps==0);
    else CHECK(!Gu.allocations&&!Gu.dma_count&&!Gu.retained);
  }
  Setup(OK);CHECK(GuDmaAllocate(8192,&cpu,&dma)==EFI_SUCCESS&&dma==0x100000&&Gu.dma_count==1);
  GuStarted();CHECK(EFI_ERROR(GuDmaFree(cpu,dma))&&Gu.retained&&Unmaps==0&&Frees==0);
  Setup(OK);CHECK(GuDmaAllocate(8192,&cpu,&dma)==EFI_SUCCESS);GuStarted();GuQuiesced();CHECK(GuDmaFree(cpu,dma)==EFI_SUCCESS&&!Gu.allocations&&!Gu.dma_count);
  Setup(OK);Pci.Map=NULL;CHECK(EFI_ERROR(GuDmaAllocate(8192,&cpu,&dma))&&Allocs==0);
}
static void TestStopContainment(void){
  Setup(OK);CHECK(GuRestoreEntryCommandZero()==EFI_SUCCESS&&U16(Config+4)==0);
  unsigned before=Writes;
  CHECK(GuContainMasterForStop()==EFI_SUCCESS&&Writes==before&&!Gu.fault&&!Gu.retained);
  /* Working recovery must still reject zero; the STOP exception cannot leak. */
  CHECK(EFI_ERROR(GuClearMaster())&&Gu.fault&&Gu.retained);
  Setup(OK);CHECK(GuEnableMaster()==EFI_SUCCESS&&U16(Config+4)==6);
  CHECK(GuContainMasterForStop()==EFI_SUCCESS&&U16(Config+4)==2&&!Gu.master_attributes_pending);
  Setup(OK);Put(4,0,2);Gu.fault=Gu.retained=TRUE;
  CHECK(GuContainMasterForStop()==EFI_SUCCESS&&Gu.fault&&Gu.retained&&Writes==0);
  Setup(WRONG_ID);CHECK(EFI_ERROR(GuContainMasterForStop())&&Writes==0);
  Setup(OK);CHECK(GuEnableMaster()==EFI_SUCCESS);Cached=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
  CHECK(EFI_ERROR(GuContainMasterForStop())&&U16(Config+4)==2&&Gu.fault&&Gu.retained);
  Setup(CLEAR_IGNORED);CHECK(GuEnableMaster()==EFI_SUCCESS);
  CHECK(EFI_ERROR(GuContainMasterForStop())&&U16(Config+4)==6&&Gu.fault&&Gu.retained);
}
static void TestFlr(void){unsigned mode;EFI_STATUS s;UINTN i;
  for(mode=OK;mode<=RESTORE_ROM_IGNORED;mode++) {
    if(mode!=OK&&mode<FLR_FAIL)continue;
    Setup(mode);CHECK(GuSavePci()==EFI_SUCCESS);s=GuFlr();
    if(mode==FLR_FAIL||mode==FLR_PENDING||mode==FLR_ID_GONE){CHECK(EFI_ERROR(s));if(mode==FLR_PENDING)CHECK(FlrWrites==0);else CHECK(Stalled>=100000);continue;}
    CHECK(s==EFI_SUCCESS&&Gu.flrs==1&&Gu.started&&!Gu.quiesced);s=GuRestorePci();
    if(mode==RESTORE_BAR_IGNORED||mode==RESTORE_ROM_IGNORED)CHECK(EFI_ERROR(s));
    else CHECK(s==EFI_SUCCESS&&memcmp(Config,Initial,256)==0);
    for(i=0;i<6;i++)CHECK(WriteOffsets[2+i]==0x24-4*i);
    CHECK(EFI_ERROR(GuFlr()));
  }
}
int main(void){TestMaster();TestDma();TestFlr();TestStopContainment();Setup(OK);printf("PASS: %u port assertions; direct command, DMA retention, FLR restore, STOP at command zero; host mocks only.\n",Checks);return 0;}
