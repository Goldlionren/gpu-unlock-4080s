#include "port.h"
VOID *memset(VOID *, int, size_t);
VOID *memcpy(VOID *, const VOID *, size_t);
GU_PLATFORM Gu;
VOID GuInit(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci) {
  memset(&Gu,0,sizeof(Gu)); Gu.bs=Bs; Gu.pci=Pci;
}
BOOLEAN GuTargetConfig(const UINT32 *C, UINTN S, UINTN B, UINTN D, UINTN F) {
  return C && !S && B==1 && !D && !F && C[0]==GU_BOARD_ID &&
    C[11]==GU_BOARD_SUBSYSTEM && (C[2]>>24)==3 && !((C[3]>>16)&0x7f);
}
BOOLEAN GuSafeToReturn(VOID) {
  return !Gu.retained && !Gu.master_attributes_pending &&
    (!Gu.started || (Gu.quiesced && !Gu.fault));
}
VOID *GuAlloc(size_t N) {
  VOID *Memory=NULL;
  if(!N || !Gu.bs || EFI_ERROR(Gu.bs->AllocatePool(EfiLoaderData,N,&Memory)))return NULL;
  memset(Memory,0,N);return Memory;
}
VOID GuFree(VOID *Memory) {
  /* Pool scratch is CPU-only; DMA uses GuDmaFree's separate lifetime guard. */
  if(Memory && !Gu.retained)Gu.bs->FreePool(Memory);
}
VOID GuStarted(VOID) {Gu.started=TRUE;Gu.quiesced=FALSE;}
VOID GuQuiesced(VOID) {if(!Gu.fault)Gu.quiesced=TRUE;}
EFI_STATUS GuDmaAllocate(UINTN N, VOID **C, UINT64 *D) {
  EFI_STATUS Status; GU_DMA *Entry; UINTN Mapped;
  if(!C || !D)return EFI_INVALID_PARAMETER;
  *C=NULL;*D=0;
  if(!N || N>(128U<<20) || Gu.fault || Gu.retained || !Gu.pci || !Gu.owned ||
     !Gu.pci->AllocateBuffer || !Gu.pci->FreeBuffer || !Gu.pci->Map || !Gu.pci->Unmap)return EFI_INVALID_PARAMETER;
  Entry=GuAlloc(sizeof(*Entry));if(!Entry)return EFI_OUT_OF_RESOURCES;
  Entry->pages=(N+4095)/4096;Entry->bytes=Entry->pages*4096;
  Status=Gu.pci->AllocateBuffer(Gu.pci,AllocateAnyPages,EfiBootServicesData,
    Entry->pages,&Entry->cpu,0);
  if(EFI_ERROR(Status) || !Entry->cpu) {
    /* An output pointer on error has uncertain ownership; never free guessed state. */
    if(Entry->cpu){Entry->next=Gu.allocations;Gu.allocations=Entry;Gu.retained=TRUE;}
    else GuFree(Entry);
    return EFI_ERROR(Status)?Status:EFI_OUT_OF_RESOURCES;
  }
  memset(Entry->cpu,0,Entry->bytes);Mapped=Entry->bytes;
  Status=Gu.pci->Map(Gu.pci,EfiPciIoOperationBusMasterCommonBuffer,Entry->cpu,
    &Mapped,&Entry->dma,&Entry->mapping);
  if(EFI_ERROR(Status) && Entry->mapping) {
    /* No documented valid mapping on error: retain rather than risk freeing a
       buffer still reachable by a partly established IOMMU mapping. */
    Entry->next=Gu.allocations;Gu.allocations=Entry;Gu.retained=TRUE;return Status;
  }
  if(!EFI_ERROR(Status))Entry->mapped=TRUE;
  if(EFI_ERROR(Status) || Mapped!=Entry->bytes || !Entry->dma ||
     (Entry->dma&4095) || Entry->dma>0xffffffffULL || Entry->bytes-1>0xffffffffULL-Entry->dma) {
    if(!EFI_ERROR(Status))Status=EFI_UNSUPPORTED;
    if(Entry->mapped && EFI_ERROR(Gu.pci->Unmap(Gu.pci,Entry->mapping))) {
      Entry->next=Gu.allocations;Gu.allocations=Entry;Gu.retained=TRUE;return Status;
    }
    if(EFI_ERROR(Gu.pci->FreeBuffer(Gu.pci,Entry->pages,Entry->cpu))) {
      Entry->mapped=FALSE;Entry->next=Gu.allocations;Gu.allocations=Entry;Gu.retained=TRUE;return Status;
    }
    GuFree(Entry);return Status;
  }
  Entry->next=Gu.allocations;Gu.allocations=Entry;Gu.dma_count++;
  *C=Entry->cpu;*D=Entry->dma;
  if(!(Gu.dma_count%2048))GuLog("DMA buffers mapped: %u\n",Gu.dma_count);
  return EFI_SUCCESS;
}
EFI_STATUS GuDmaFree(VOID *C, UINT64 D) {
  GU_DMA **Link=&Gu.allocations,*Entry;EFI_STATUS Status;
  while(*Link && ((*Link)->cpu!=C || (*Link)->dma!=D))Link=&(*Link)->next;
  if(!*Link)return EFI_NOT_FOUND;
  Entry=*Link;
  if(Gu.retained || Gu.master_attributes_pending || (Gu.started && (!Gu.quiesced || Gu.fault))) {
    Gu.retained=TRUE;return EFI_ACCESS_DENIED;
  }
  if(Entry->mapped) {
    Status=Gu.pci->Unmap(Gu.pci,Entry->mapping);
    if(EFI_ERROR(Status)) {Gu.retained=TRUE;return Status;}
    Entry->mapped=FALSE;
  }
  Status=Gu.pci->FreeBuffer(Gu.pci,Entry->pages,Entry->cpu);
  if(EFI_ERROR(Status)) {Gu.retained=TRUE;return Status;}
  *Link=Entry->next;GuFree(Entry);if(Gu.dma_count)Gu.dma_count--;return EFI_SUCCESS;
}
UINT32 GuRead32(UINT32 O) {
  UINT32 Value=0xffffffffU;
  if(Gu.fault)return Value;
  if(!Gu.pci || (O&3) || (UINT64)O+4>Gu.bar_size ||
     EFI_ERROR(Gu.pci->Mem.Read(Gu.pci,EfiPciIoWidthUint32,0,O,1,&Value))) {
    Gu.fault=TRUE;return 0xffffffffU;
  }
  return Value;
}
VOID GuWrite32(UINT32 O, UINT32 V) {
  if(Gu.fault)return;
  if(!Gu.pci || !Gu.armed || !Gu.owned || !Gu.display_ok || (O&3) ||
     (UINT64)O+4>Gu.bar_size) {Gu.fault=TRUE;return;}
  Gu.mmio_writes++;
  if(EFI_ERROR(Gu.pci->Mem.Write(Gu.pci,EfiPciIoWidthUint32,0,O,1,&V)))Gu.fault=TRUE;
}
static EFI_STATUS RawRead(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
  if(!Gu.pci || !Gu.pci->Pci.Read || !V || (UINT32)W>EfiPciIoWidthUint32 || O>256-((UINT32)1<<W))return EFI_INVALID_PARAMETER;
  return Gu.pci->Pci.Read(Gu.pci,W,O,1,V);
}
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
  EFI_STATUS Status;
  if(Gu.fault)return EFI_DEVICE_ERROR;
  Status=RawRead(W,O,V);if(EFI_ERROR(Status))Gu.fault=TRUE;return Status;
}
EFI_STATUS GuConfigWrite(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
  EFI_STATUS Status;
  if(Gu.fault)return EFI_DEVICE_ERROR;
  if(!Gu.pci || !Gu.pci->Pci.Write || !V || !Gu.owned || !Gu.armed || !Gu.display_ok || (UINT32)W>EfiPciIoWidthUint32 ||
     O>256-((UINT32)1<<W)) {Gu.fault=TRUE;return EFI_ACCESS_DENIED;}
  Status=Gu.pci->Pci.Write(Gu.pci,W,O,1,V);
  if(EFI_ERROR(Status))Gu.fault=TRUE;return Status;
}
VOID GuStall(UINTN Microseconds) {
  Gu.bs->Stall(Microseconds);Gu.ticks+=Microseconds;
}
EFI_STATUS GuEnableMaster(VOID) {
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff;
  UINT64 Supported=0,Before=0,After=0;
  EFI_STATUS Status=EFI_ACCESS_DENIED;
  if(Gu.fault || Gu.retained || Gu.started || Gu.master_attributes_pending ||
     !Gu.pci || !Gu.pci->Pci.Write || !Gu.pci->Attributes || !Gu.owned || !Gu.armed || !Gu.display_ok)goto fail;
  Status=RawRead(EfiPciIoWidthUint32,0,&Id);
  if(EFI_ERROR(Status) || Id!=GU_BOARD_ID)goto fail;
  Status=RawRead(EfiPciIoWidthUint16,4,&Command);
  if(EFI_ERROR(Status) || Command!=2)goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationSupported,0,&Supported);
  if(EFI_ERROR(Status) || !(Supported&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Before);
  /* This board enters with Attributes cache=0. All temporary decode/master
     changes are endpoint command writes; never recurse through Attributes. */
  if(EFI_ERROR(Status) || Before!=0)goto fail;
  GuLog("Bus Master enable intent: command=%#x attributes=%#llx\n",
    Command,(unsigned long long)Before);
  if(Gu.fault){Status=EFI_DEVICE_ERROR;goto fail;}
  Gu.master_original_attributes=Before;
  Gu.master_original_command=Command;
  Gu.master_pci=Gu.pci;
  /* Track before the call: an error may be returned after a hardware change. */
  Gu.master_attributes_pending=TRUE;
  Now=6;
  Status=Gu.pci->Pci.Write(Gu.pci,EfiPciIoWidthUint16,4,1,&Now);
  if(EFI_ERROR(Status))goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&After);
  if(EFI_ERROR(Status) || After!=Before)goto fail;
  Status=RawRead(EfiPciIoWidthUint16,4,&Now);
  if(EFI_ERROR(Status) || Now!=(UINT16)(Command|4))goto fail;
  GuLog("Bus Master enabled: command=%#x attributes=%#llx matched=1\n",
    Now,(unsigned long long)After);
  if(Gu.fault){Status=EFI_DEVICE_ERROR;goto fail;}
  return EFI_SUCCESS;
fail:
  Gu.fault=TRUE;
  if(!EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  GuLog("Bus Master enable failed: status=%#llx command=%#x readback=%#x managed=%u\n",
    (unsigned long long)Status,Command,Now,Gu.master_attributes_pending);
  return Status;
}
static EFI_STATUS ClearMaster(BOOLEAN Stopping) {
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff;EFI_STATUS Status;
  EFI_STATUS AttributeStatus=EFI_SUCCESS;UINT64 Attributes=~0ULL;
  /* Containment remains permitted after a latched I/O failure. */
  Status=RawRead(EfiPciIoWidthUint32,0,&Id);
  if(EFI_ERROR(Status) || Id!=GU_BOARD_ID)goto fail;
  if(!Gu.pci->Pci.Write) {Status=EFI_UNSUPPORTED;goto fail;}
  if(Gu.master_attributes_pending && Gu.pci!=Gu.master_pci)AttributeStatus=EFI_COMPROMISED_DATA;
  else if(!Gu.pci->Attributes)AttributeStatus=EFI_UNSUPPORTED;
  else {
    AttributeStatus=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Attributes);
    if(!EFI_ERROR(AttributeStatus) && Attributes!=0)AttributeStatus=EFI_DEVICE_ERROR;
  }
  Gu.master_last_attributes=Attributes;
  if(EFI_ERROR(AttributeStatus) && !EFI_ERROR(Gu.master_attribute_status))
    Gu.master_attribute_status=AttributeStatus;
  /* Always clear hardware even after a cache error or a latched I/O fault. */
  Status=RawRead(EfiPciIoWidthUint16,4,&Command);
  if(EFI_ERROR(Status) || Command==0xffff)goto fail;
  if(Command&4) {
    Command&=0xfffbU;
    Status=Gu.pci->Pci.Write(Gu.pci,EfiPciIoWidthUint16,4,1,&Command);
    if(EFI_ERROR(Status))goto fail;
  }
  Status=RawRead(EfiPciIoWidthUint16,4,&Now);
  if(EFI_ERROR(Status) || Now==0xffff || (Now&4))goto fail;
  /* Core recovery requires MEM-only. STOP may run after the final 2->0
     restoration; command zero is already contained and needs no write.
     This exception never grants permission to hand off or free resources. */
  if(Stopping) {if(Now!=0 && Now!=2)goto fail;}
  else if(Now!=2 || (Gu.master_attributes_pending && Now!=Gu.master_original_command))goto fail;
  Gu.master_last_command=Now;
  if(EFI_ERROR(AttributeStatus)) {
    Gu.fault=TRUE;Gu.retained=TRUE;return AttributeStatus;
  }
  Gu.master_attributes_pending=FALSE;
  return EFI_SUCCESS;
fail:
  Gu.master_last_command=Now;
  if(!EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(Gu.master_hardware_status))Gu.master_hardware_status=Status;
  Gu.fault=TRUE;Gu.retained=TRUE;return Status;
}
EFI_STATUS GuClearMaster(VOID) {return ClearMaster(FALSE);}
EFI_STATUS GuContainMasterForStop(VOID) {return ClearMaster(TRUE);}
EFI_STATUS GuRestoreEntryCommandZero(VOID) {
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff,Zero=0;UINT64 Attributes=~0ULL;EFI_STATUS Status;
  const char *Step="identity";
  BOOLEAN Complete=FALSE;
  /* Raw containment is permitted after fault. DMA must already be disposed of;
     never turn off MEM while engine cleanup still needs BAR access. */
  if(!Gu.pci || !Gu.pci->Pci.Write || !Gu.pci->Attributes || !Gu.owned ||
     Gu.retained || Gu.allocations || Gu.dma_count || Gu.master_attributes_pending ||
     (Gu.started && !Gu.quiesced)) {
    Step="precondition";Status=EFI_ACCESS_DENIED;goto record;
  }
  Status=RawRead(EfiPciIoWidthUint32,0,&Id);if(EFI_ERROR(Status)||Id!=GU_BOARD_ID)goto fail;
  Step="command-before";
  Status=RawRead(EfiPciIoWidthUint16,4,&Command);if(EFI_ERROR(Status)||(Command!=0&&Command!=2))goto fail;
  Step="attributes-before";
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Attributes);
  if(EFI_ERROR(Status)||Attributes!=0)goto fail;
  if(Command!=0) {
    Step="command-write-zero";
    Status=Gu.pci->Pci.Write(Gu.pci,EfiPciIoWidthUint16,4,1,&Zero);
    if(EFI_ERROR(Status))goto fail;
  }
  Step="command-after";
  Status=RawRead(EfiPciIoWidthUint16,4,&Now);if(EFI_ERROR(Status)||Now!=0)goto fail;
  Step="attributes-after";
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Attributes);
  if(EFI_ERROR(Status)||Attributes!=0)goto fail;
  Step="complete";Complete=TRUE;goto record;
fail:
  Gu.fault=TRUE;Gu.retained=TRUE;
record:
  Gu.entry_restore_step=Step;Gu.entry_restore_api_status=Status;
  Gu.entry_restore_id=Id;Gu.entry_restore_before=Command;Gu.entry_restore_after=Now;Gu.entry_restore_attributes=Attributes;
  return Complete?EFI_SUCCESS:(EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR);
}
EFI_STATUS GuSavePci(VOID) {
  UINTN I;
  if(Gu.saved || Gu.fault || !Gu.owned || Gu.pcie_cap<0x40 || Gu.pcie_cap>0xd4)return EFI_NOT_READY;
  for(I=0;I<64;I++)
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)I*4,&Gu.saved_config[I])))return EFI_DEVICE_ERROR;
  if(Gu.saved_config[0]!=GU_BOARD_ID || Gu.saved_config[11]!=GU_BOARD_SUBSYSTEM ||
     (UINT16)Gu.saved_config[1]!=2)return EFI_ACCESS_DENIED;
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Gu.saved_devctl)))return EFI_DEVICE_ERROR;
  if(Gu.saved_devctl&0x8000)return EFI_NOT_READY;
  if(Gu.pcie_version>=2 && EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,
     Gu.pcie_cap+0x28,&Gu.saved_devctl2)))return EFI_DEVICE_ERROR;
  Gu.saved=TRUE;return EFI_SUCCESS;
}
EFI_STATUS GuFlr(VOID) {
  UINT32 Identity=0,Capability=0;UINT16 Command=0xffff,Pending=0xffff,Ctl=0;
  UINTN Wait;EFI_STATUS ResetStatus;
  if(!Gu.saved || Gu.flrs || Gu.fault || !Gu.owned || !Gu.armed || !Gu.display_ok)return EFI_NOT_READY;
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,0,&Identity)) || Identity!=GU_BOARD_ID ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,4,&Command)) || Command==0xffff ||
     (Command&4) || !(Command&2) ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,Gu.pcie_cap+4,&Capability)) ||
     !(Capability&(1U<<28)))return EFI_ACCESS_DENIED;
  for(Wait=0;Wait<=1000000;Wait+=1000) {
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+10,&Pending)) || Pending==0xffff)return EFI_DEVICE_ERROR;
    if(!(Pending&0x20))break;
    if(Wait==1000000)return EFI_TIMEOUT;
    GuStall(1000);
  }
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Ctl)))return EFI_DEVICE_ERROR;
  if(Ctl&0x8000)return EFI_NOT_READY;
  Ctl|=0x8000;Gu.flrs++;GuStarted();
  ResetStatus=GuConfigWrite(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Ctl);
  /* No access of any kind to this function for at least 100 ms. */
  GuStall(100000);
  if(EFI_ERROR(ResetStatus))return EFI_DEVICE_ERROR;
  for(Wait=0;Wait<=1000000;Wait+=1000) {
    EFI_STATUS Status=RawRead(EfiPciIoWidthUint32,0,&Identity);
    if(!EFI_ERROR(Status) && Identity==GU_BOARD_ID)return EFI_SUCCESS;
    if(!EFI_ERROR(Status) && Identity!=0xffffffffU && Identity!=0)break;
    if(Wait<1000000)GuStall(1000);
  }
  Gu.fault=TRUE;return EFI_DEVICE_ERROR;
}
EFI_STATUS GuRestorePci(VOID) {
  UINT16 Command=0,BytePair=0;UINT32 Value=0;UINT8 Line;INTN Offset;
  if(!Gu.saved || Gu.flrs!=1 || Gu.fault)return EFI_NOT_READY;
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,4,&Command)) || Command==0xffff || (Command&4))return EFI_DEVICE_ERROR;
  Command&=0xfff8U;
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,4,&Command)) ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,4,&BytePair)) || (BytePair&7))return EFI_DEVICE_ERROR;
  /* Decode disabled; restore high BAR dwords before their low dwords. */
  for(Offset=0x24;Offset>=0x10;Offset-=4) {
    Value=Gu.saved_config[Offset/4];
    if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint32,(UINT32)Offset,&Value)))return EFI_DEVICE_ERROR;
  }
  Value=Gu.saved_config[0x30/4]&~1U; /* Never enable expansion ROM decoding. */
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint32,0x30,&Value)))return EFI_DEVICE_ERROR;
  BytePair=(UINT16)Gu.saved_config[3];
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,0x0c,&BytePair)))return EFI_DEVICE_ERROR;
  Line=(UINT8)Gu.saved_config[0x3c/4];
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint8,0x3c,&Line)))return EFI_DEVICE_ERROR;
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Gu.saved_devctl)))return EFI_DEVICE_ERROR;
  if(Gu.pcie_version>=2 && EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,
     Gu.pcie_cap+0x28,&Gu.saved_devctl2)))return EFI_DEVICE_ERROR;
  Command=(UINT16)Gu.saved_config[1]&~4U;
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,4,&Command)))return EFI_DEVICE_ERROR;
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,4,&BytePair)) || BytePair!=Command)return EFI_DEVICE_ERROR;
  for(Offset=0x10;Offset<=0x24;Offset+=4) {
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)Offset,&Value)) ||
       Value!=Gu.saved_config[Offset/4])return EFI_DEVICE_ERROR;
  }
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,0,&Value)) || Value!=GU_BOARD_ID ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,0x2c,&Value)) || Value!=GU_BOARD_SUBSYSTEM ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,0x30,&Value)) || Value!=(Gu.saved_config[0x30/4]&~1U) ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,0x0c,&BytePair)) || BytePair!=(UINT16)Gu.saved_config[3] ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint8,0x3c,&Line)) || Line!=(UINT8)Gu.saved_config[0x3c/4] ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+8,&BytePair)) || BytePair!=Gu.saved_devctl)return EFI_DEVICE_ERROR;
  if(Gu.pcie_version>=2 && (EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+0x28,&BytePair)) ||
     BytePair!=Gu.saved_devctl2))return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
