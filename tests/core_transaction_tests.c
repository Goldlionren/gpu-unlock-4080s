/* SPDX-License-Identifier: GPL-2.0-only
 * Real transaction/port/compat with host-only firmware/MMIO/DMA simulation.
 * Measured 4080 SUPER identity, VRAM, PROM and disabled attribute cache fixture.
 */
#include "core_fixture.inc"
#include "core_cases.h"

int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IONBF,0);CHECK(argc==3);load_fixture_assets(argv[1],argv[2]);
 const UINTN standard_count=sizeof(ad_cases)/sizeof(*ad_cases);
 const struct {int mode;UINT32 ss0,ss1;} initial_cases[]={
  {SUCCESS,0,0},{AD_SS0_REJECT,0,0},{AD_SS1_REJECT,0,0},
  {SUCCESS,0x64113036,2},{SUCCESS,0x64113037,2}};
 const UINTN initial_count=sizeof(initial_cases)/sizeof(*initial_cases);
 for(UINTN c=0;c<standard_count+initial_count;c++) {
  int mode=c<standard_count?ad_cases[c]:initial_cases[c-standard_count].mode;setup((enum scenario)mode);
  UINT32 initialSs0=0x64113035,initialSs1=2;
  if(c>=standard_count){initialSs0=initial_cases[c-standard_count].ss0;initialSs1=initial_cases[c-standard_count].ss1;}
  registers[SS0/4]=initialSs0;registers[SS1/4]=initialSs1;
  UINT64 mask=0xffffffffffffULL;
  struct pci_dev device={.dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffULL},.pm_cap=0x60,.refs=1};
  struct gpu_unlock_610_shared shared={0};
  int result=gpu_unlock_610_load_shared(&device,&shared);CHECK(!result);
  result=gpu_unlock_610_unlock(&device,&shared);gpu_unlock_610_release_shared(&shared);
  const struct gpu_unlock_610_outcome *out=gpu_unlock_610_get_outcome();
  BOOLEAN safe=GuSafeToReturn();
  BOOLEAN selector_reject=(BOOLEAN)(mode==AD_SS0_REJECT||mode==AD_SS1_REJECT||mode==SELECTOR_LOST);
  CHECK(out->result==result);
  if(ad_success(mode)||selector_reject) {
   CHECK((result==0)==ad_success(mode));
   CHECK(safe && Gu.quiesced && !Gu.retained && !Gu.fault && out->recovered);
   CHECK(out->firmware_started && out->required_plm_verified);
   CHECK(fake.gsp_starts==1 && fake.sec2_starts==expected_sec2_starts((enum scenario)mode) && fake.flrs==1);
   CHECK(registers[SS0/4]==(mode==AD_SS0_REJECT?initialSs0:0x88888888));
   CHECK(registers[SS1/4]==(mode==AD_SS1_REJECT?initialSs1:mode==SELECTOR_LOST?2:8));
   CHECK(out->selectors_before_flr==(mode!=AD_SS0_REJECT&&mode!=AD_SS1_REJECT));
   CHECK(out->selectors_after_flr==!selector_reject);
   CHECK(out->all_plm_verified==(mode!=AD_FBPA_REJECT));
   CHECK(strstr(fake.log,"SELECTOR RESULT: SS0") && strstr(fake.log,"SELECTOR RESULT: SS1"));
   CHECK(!fake.live_maps && !fake.live_buffers && !fake.pool_live);
   CHECK(strstr(fake.log,"required_expected=1") && strstr(fake.log,"marker_ok=1"));
  } else {
   CHECK(result && !out->recovered);
   if(mode==MAP_FAIL || mode==MAP_PARTIAL || mode==BME_READBACK_FAIL) {
    CHECK(safe && !Gu.started && !Gu.retained && !fake.mmio_writes);
    CHECK(!fake.live_maps && !fake.live_buffers && !fake.pool_live);
   } else {
    CHECK(!safe && Gu.retained);
    CHECK(fake.live_maps && !fake.frees);
   }
   if(ad_before_selectors(mode))CHECK(registers[SS0/4]==initialSs0&&registers[SS1/4]==initialSs1&&!fake.flrs);
  }
  if(mode==AD_MARKER_MISSING||mode==AD_MARKER_WRONG)CHECK(strstr(fake.log,"marker_ok=0"));
  if(mode==AD_MARKER_NO_OPEN)CHECK(strstr(fake.log,"all_expected=0 marker=0x67b00001 marker_ok=1"));
  if(mode==PLM_PRIOR_RELOCK)CHECK(fake.sec2_starts==1&&!fake.flrs&&registers[SS1/4]==2);
  if(mode==SEC2_RUN_TIMEOUT||mode==PRE_RESET_RUNNING) {
   CHECK(strstr(fake.log,"Stock signature restore deferred"));
   CHECK(fake.meta_dma&&le64(dma_pointer(fake.meta_dma,256)+80)==64000);
  }
  EFI_STATUS containment=GuClearMaster();
  CHECK(!(config[4]&4));
  if(safe)CHECK(!EFI_ERROR(containment));
  CHECK(fake.gsp_starts<=1&&fake.sec2_starts<=1&&fake.flrs<=1);
  CHECK(!fake.pio_reads&&!fake.pio_writes&&!fake.dmem_reads);
  CHECK(fake.pci_attributes==0);
  printf("4080S core %s initial=%08x/%08x: result=%d safe=%u recovered=%d selectors=%d/%d starts=%zu/%zu FLR=%zu PASS\n",case_names[mode],initialSs0,initialSs1,result,safe,out->recovered,out->selectors_before_flr,out->selectors_after_flr,fake.gsp_starts,fake.sec2_starts,fake.flrs);
  dispose_host_fixture();
 }
 for(UINTN i=0;i<5;i++)free((VOID *)GuAssets[i].firmware.data);
 printf("%zu core scenarios passed; all firmware/MMIO/DMA operations simulated.\n",standard_count+initial_count);
 return 0;
}
