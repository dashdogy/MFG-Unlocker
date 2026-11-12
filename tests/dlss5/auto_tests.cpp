#include "dlss5_auto.h"
#include <cstdio>
#include <cstdlib>
unsigned checks=0;
#define CHECK(c) do{++checks;if(!(c)){std::fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#c);std::exit(1);}}while(0)
using namespace dlss5;
using namespace dlss5::auto_mode;
// Stage cost model: proportional to processed pixels.
float Cost(float fullMs,float scale){return fullMs*(scale/100.0f)*(scale/100.0f);}
// Runs the controller at 60 rendered frames per second; returns the number of level changes.
unsigned Run(ScaleController& controller,float fullMs,float budget,double seconds,size_t ceiling){
 unsigned steps=0;
 for(double t=0;t<seconds;t+=1.0/60.0)steps+=controller.Update(Cost(fullMs,controller.Scale()),budget,1.0/60.0,ceiling);
 return steps;
}
// autoscale.1: a GPU-bound game whose frame is `rest` ms without DLSS 5, and a
// stage with a fixed part (the FF7 Rebirth log, RTX 3080 Ti at 1080p: 7.06 ms
// at 100 %, 3.91 ms at 50 %). The budget comes from the measured frame and stage
// as Harvest computes it. Returns the number of level changes.
float StageMs(float scale){return 2.86f+4.20f*(scale/100.0f)*(scale/100.0f);}
unsigned RunFrame(ScaleController& controller,float rest,double seconds,size_t ceiling,float* budget=nullptr){
 unsigned steps=0;
 for(double t=0;t<seconds;t+=1.0/60.0){
  const float stage=StageMs(controller.Scale()),limit=BudgetMs(rest+stage,stage);
  if(budget)*budget=limit;
  steps+=controller.Update(stage,limit,1.0/60.0,ceiling);
 }
 return steps;
}
int main(){
 CHECK(AutoStage()==Stage::eUpscaled);
 CHECK(kLevels[StartingLevel(1920,1080)]==100.0f);CHECK(kLevels[StartingLevel(2560,1440)]==100.0f);
 CHECK(kLevels[StartingLevel(3840,2160)]==75.0f);CHECK(kLevels[StartingLevel(7680,4320)]==75.0f);
 CHECK(CeilingLevel(200)==0);CHECK(CeilingLevel(100)==0);CHECK(kLevels[CeilingLevel(80)]==75.0f);
 CHECK(kLevels[CeilingLevel(66)]==66.0f);CHECK(kLevels[CeilingLevel(10)]==33.0f);
 CHECK(BudgetMs(0)==kMaxBudgetMs);CHECK(std::abs(BudgetMs(1000.0f/60.0f)-5.0f)<1e-4f);CHECK(BudgetMs(1.0f)==kMinBudgetMs);CHECK(BudgetMs(100.0f)==kMaxBudgetMs);
 // The quality floor is 75 %; a slider below it is the floor itself.
 CHECK(kLevels[kFloorLevel]==75.0f&&FloorLevel(0)==kFloorLevel&&FloorLevel(CeilingLevel(85))==kFloorLevel);
 CHECK(FloorLevel(CeilingLevel(50))==CeilingLevel(50)&&FloorLevel(99)==kLevels.size()-1);
 // autorect.1: Auto's feature is the slider's level (never above 100 %), whatever level Auto runs at.
 CHECK(CapacityScale(200)==100.0f&&CapacityScale(100)==100.0f&&CapacityScale(90)==85.0f);
 CHECK(CapacityScale(75)==75.0f&&CapacityScale(50)==50.0f&&CapacityScale(10)==33.0f);
 // Boost's look model textures hold the most cells of any level from the ceiling down to the floor.
 {
  BoostGrid all{};
  // 4K: 100, 85 and 75 % all have 32-texel cells; 100 % has the most (120x68).
  CHECK(BoostGridCapacity(3840,2160,0,all)&&all.cell==32&&all.cellsX==120&&all.cellsY==68&&all.bins==kBoostBins);
  // 3200x1800: 100 % has 32-texel cells (100x57), 85 % 16-texel cells (170x96), 75 % (150x85).
  CHECK(BoostGridCapacity(3200,1800,0,all)&&all.cellsX==170&&all.cellsY==96);
  // A slider below the floor: its own level only (66 % of 4K: 2534x1426, 16-texel cells).
  CHECK(BoostGridCapacity(3840,2160,CeilingLevel(66),all)&&all.cell==16&&all.cellsX==159&&all.cellsY==90);
  // The CTest's 64x64 stage: 4x4 cells at 100 and 85 %, 3x3 at 75 %.
  CHECK(BoostGridCapacity(64,64,0,all)&&all.cellsX==4&&all.cellsY==4);
  CHECK(!BoostGridCapacity(0,64,0,all)&&!BoostGridCapacity(kMaxExtent+1,64,0,all));
 }
 // The budget comes from the frame without the stage: the same at every level of
 // the FF7 Rebirth case.
 for(const float scale:kLevels){
  const float budget=BudgetMs(5.61f+StageMs(scale),StageMs(scale));
  CHECK(std::abs(budget-5.61f*kBudgetShare/(1.0f-kBudgetShare))<1e-3f);
 }
 CHECK(BudgetMs(10.0f,12.0f)==kMinBudgetMs&&BudgetMs(10.0f,std::nanf(""))==BudgetMs(10.0f));
 // autofps.1: Dynamic frame generation's target. 116 FPS presented is an 8.62 ms interval.
 {
  const auto near=[](float a,double b){return std::abs(double(a)-b)<1e-3;};
  const double interval=1000.0/116.0;
  CHECK(DynamicBudgetMs(30.0f,6.0f,0.0f,2,6)==BudgetMs(30.0f,6.0f)&&DynamicBudgetMs(30.0f,6.0f,116.0f,2,0)==BudgetMs(30.0f,6.0f));
  CHECK(DynamicBudgetMs(0.0f,6.0f,116.0f,2,6)==BudgetMs(0.0f,6.0f)&&DynamicBudgetMs(30.0f,-1.0f,116.0f,2,6)==BudgetMs(30.0f,-1.0f));
  CHECK(DynamicBudgetMs(30.0f,6.0f,std::nanf(""),2,6)==BudgetMs(30.0f,6.0f));
  // A light game (10 ms without DLSS 5): Dynamic stays at its 2x minimum up to 17.24 ms, so DLSS 5 may take the
  // 7.24 ms left before it needs 3x, more than the 4.29 ms the 30 % rule gives.
  CHECK(BudgetMs(12.0f,2.0f)<5.0f&&near(DynamicBudgetMs(12.0f,2.0f,116.0f,2,6),2*interval-10.0));
  // With 1X allowed (XeMFG), 10 ms already needs 2 frames: the same slack.
  CHECK(near(DynamicBudgetMs(12.0f,2.0f,116.0f,1,6),2*interval-10.0));
  // 6 ms without DLSS 5 with 1X allowed: 1X holds up to 8.62 ms, so 2.62 ms of slack, more than the 30 % rule's 2.57.
  CHECK(near(DynamicBudgetMs(8.0f,2.0f,116.0f,1,6),interval-6.0));
  // A heavy game (28 ms without DLSS 5, 4 frames needed): the 30 % rule's 12 ms is more than the 6.48 ms of slack
  // and within what 6 frames still reach (23.7 ms): 12 ms, as without a target.
  CHECK(DynamicBudgetMs(37.0f,9.0f,116.0f,2,6)==kMaxBudgetMs);
  // Up to 4 frames only: the target caps it at 4 x 8.62 - 28 = 6.48 ms.
  CHECK(near(DynamicBudgetMs(37.0f,9.0f,116.0f,2,4),4*interval-28.0));
  // The frame misses the target even without DLSS 5 (50 ms, 4 frames reach 34.5 ms): 0, Auto holds its floor.
  CHECK(DynamicBudgetMs(58.0f,8.0f,116.0f,2,4)==0.0f);
  // Right at the edge: 6 frames reach 51.7 ms, 50 ms without DLSS 5 leaves 1.72 ms, under the 30 % rule's 12.
  CHECK(near(DynamicBudgetMs(58.0f,8.0f,116.0f,2,6),6*interval-50.0));
  // The budget does not move with the stage's own cost (the frame without it stays the same).
  CHECK(near(DynamicBudgetMs(16.0f,6.0f,116.0f,2,6),DynamicBudgetMs(13.0f,3.0f,116.0f,2,6)));
 }
 // Overload: no change during the minimum interval, then one step per interval.
 ScaleController c;c.Reset(0);
 CHECK(Run(c,10.0f,5.0f,7.9,0)==0&&c.level==0);
 CHECK(Run(c,10.0f,5.0f,0.2,0)==1&&c.level==1);
 CHECK(Run(c,10.0f,5.0f,7.0,0)==0&&c.level==1);
 // Converges inside the band (10 ms at native, 5 ms budget: 75 % = 5.6 ms is within
 // 15 % of the budget, 85 % would not fit) and stays.
 c.Reset(0);const unsigned steps=Run(c,10.0f,5.0f,120.0,0);
 CHECK(c.Scale()==75.0f&&steps==2);
 CHECK(Run(c,10.0f,5.0f,120.0,0)==0&&c.Scale()==75.0f);
 // A heavier stage stops at the floor (14 ms native: 75 % = 7.9 ms is still over the band)
 // and says so; the frame rate pays, not the detail.
 c.Reset(0);CHECK(Run(c,14.0f,5.0f,120.0,0)==2&&c.Scale()==75.0f&&c.floorHeld);
 // More headroom: back up, one level per interval, not above the ceiling.
 CHECK(Run(c,4.0f,5.0f,60.0,0)>=1&&c.Scale()==100.0f);
 c.Reset(0);CHECK(Run(c,1.0f,5.0f,30.0,CeilingLevel(75))>=1&&c.Scale()==75.0f);
 CHECK(Run(c,1.0f,5.0f,60.0,CeilingLevel(75))==0&&c.Scale()==75.0f);
 // Lowering the slider takes effect at once.
 c.Reset(0);CHECK(c.Update(1.0f,5.0f,0.016,CeilingLevel(50))&&c.Scale()==50.0f);
 // A slider below the floor: Auto holds the slider's level, never below it, and the
 // floor note stays off (the user chose that scale).
 c.Reset(0);CHECK(Run(c,100.0f,2.0f,60.0,CeilingLevel(50))==1&&c.Scale()==50.0f&&!c.floorHeld);
 // A level below the floor (an older controller state) comes back up to it.
 c.Reset(kLevels.size()-1);CHECK(c.Clamp(0)&&c.Scale()==75.0f);
 c.Reset(kLevels.size()-1);CHECK(Run(c,100.0f,2.0f,60.0,0)==1&&c.Scale()==75.0f&&c.floorHeld);
 // Invalid measurements ignored.
 c.Reset(0);CHECK(!c.Update(-1.0f,5.0f,0.016,0)&&!c.Update(1.0f,5.0f,-1.0,0)&&c.samples==0);
 // The FF7 Rebirth case: from 100 % to the floor in two steps and held there,
 // with a budget that stays put.
 {
  ScaleController ff7;ff7.Reset(0);float budget=0.0f;
  CHECK(RunFrame(ff7,5.61f,120.0,0,&budget)==2&&ff7.Scale()==75.0f&&ff7.floorHeld);
  CHECK(std::abs(budget-2.40f)<0.01f);
  CHECK(RunFrame(ff7,5.61f,120.0,0)==0&&ff7.Scale()==75.0f&&ff7.floorHeld);
  // A heavier scene gives a larger budget: up again (85 % fits, 100 % would not), floor note off.
  CHECK(RunFrame(ff7,20.0f,60.0,0,&budget)==1&&ff7.Scale()==85.0f&&!ff7.floorHeld&&budget>8.0f);
  CHECK(RunFrame(ff7,20.0f,60.0,0)==0&&ff7.Scale()==85.0f);
  // Plenty of headroom: back to the slider's 100 %, never above it.
  CHECK(RunFrame(ff7,40.0f,60.0,0)==1&&ff7.Scale()==100.0f);
 }
 // nrscale.1: a step up that does not hold (e.g. over the game's video memory budget) waits 30 s before the next
 // try, then 60, 120 ... up to 600 s, instead of every 16 s; a step up that holds 30 s clears the wait.
 {
  ScaleController s;s.Reset(1);unsigned ups=0,downs=0;
  for(double t=0;t<300.0;t+=1.0/60.0){
   const float before=s.Scale();
   if(s.Update(s.Scale()>=100.0f?12.0f:3.0f,5.0f,1.0/60.0,0))++(s.Scale()>before?ups:downs);
  }
  CHECK(ups==4&&downs==4&&s.Scale()==85.0f&&s.upWait==240.0);
  CHECK(Run(s,3.0f,5.0f,300.0,0)==1&&s.Scale()==100.0f&&s.upWait==0.0&&!s.steppedUp);
  // Lowering the slider is no failed step up, and a step down without a step up before it waits nothing.
  s.Reset(0);CHECK(Run(s,7.0f,5.0f,20.0,0)==1&&s.Scale()==85.0f&&s.upWait==0.0);
  CHECK(Run(s,3.0f,5.0f,10.0,0)==1&&s.Scale()==100.0f&&s.steppedUp);
  CHECK(s.Update(1.0f,5.0f,0.016,CeilingLevel(75))&&s.Scale()==75.0f&&s.upWait==0.0&&!s.steppedUp);
 }
 // nrscale.1: a replacement's video memory from a measured create, grown with the extent's area, never
 // shrunk; it goes beside the live feature only within the budget, and unknown memory keeps it beside.
 // The warproot.1 run in Subnautica 2: one feature at 4K left 11926 MB in use of a 12658 MB budget.
 {
  constexpr uint64_t MB=1ull<<20;
  CHECK(ReplacementBytes(600*MB,3072,1728,3840,2160)==983040000ull);
  CHECK(ReplacementBytes(600*MB,3840,2160,3072,1728)==600*MB&&ReplacementBytes(600*MB,0,0,3840,2160)==600*MB);
  CHECK(ReplacementBytes(0,3072,1728,3840,2160)==0);
  CHECK(!RoomBeside(11926*MB,12658*MB,983040000ull)&&RoomBeside(11926*MB,12658*MB,600*MB));
  CHECK(!RoomBeside(13244*MB,12641*MB,1));
  CHECK(RoomBeside(13244*MB,0,600*MB)&&RoomBeside(11926*MB,12658*MB,0));
 }
 // Keeping: the live feature's creation settings, everything else from the new ones.
 {
  Settings applied{};applied.scale=100.0f;applied.model=0;applied.passes=1;applied.preset=0;
  Settings next=applied;next.scale=75.0f;next.model=1;next.passes=2;next.preset=2;next.intensity=0.3f;next.structure=0.9f;
  const Settings live=Keeping(next,applied);
  CHECK(SameFeature(live,applied)&&live.intensity==0.3f&&live.structure==0.9f&&!SameFeature(next,applied));
 }
 // Exposure: first measurement sets the gain, later ones smooth toward middle grey, clamped.
 ExposureController e;e.Update(std::log2(0.18f),0.016);CHECK(e.valid&&std::abs(e.Gain()-1.0f)<1e-5f);
 e.Reset();e.Update(std::log2(0.045f),0.016);CHECK(std::abs(e.Gain()-4.0f)<1e-4f);
 e.Update(std::log2(0.18f),0.1);CHECK(e.Gain()<4.0f&&e.Gain()>1.0f);
 for(int n=0;n<200;++n)e.Update(std::log2(0.18f),0.05);CHECK(std::abs(e.Gain()-1.0f)<1e-3f);
 e.Reset();e.Update(std::log2(1e-6f),0.016);CHECK(e.Gain()==ExposureController::kMaxGain);
 e.Reset();e.Update(std::log2(1e6f),0.016);CHECK(e.Gain()==ExposureController::kMinGain);
 e.Reset();e.Update(std::nanf(""),0.016);CHECK(!e.valid&&e.Gain()==1.0f);
 // Adaptive target (dlss5adv.1): the luminance the gain aims for; an invalid one falls back to middle grey.
 e.Reset();e.Update(std::log2(0.09f),0.016,0.09f);CHECK(std::abs(e.Gain()-1.0f)<1e-5f);
 e.Reset();e.Update(std::log2(0.09f),0.016,0.36f);CHECK(std::abs(e.Gain()-4.0f)<1e-4f);
 e.Reset();e.Update(std::log2(0.18f),0.016,-1.0f);CHECK(std::abs(e.Gain()-1.0f)<1e-5f);
 // Frame clock: steady intervals, pauses ignored.
 FrameClock clock;CHECK(clock.Tick(1000,1000)==0.0);CHECK(std::abs(clock.Tick(1016,1000)-0.016)<1e-9);
 CHECK(clock.averageMs==16.0f);CHECK(clock.Tick(2016,1000)==0.0&&clock.averageMs==16.0f);
 CHECK(clock.Tick(2032,1000)>0.0);CHECK(clock.Tick(2032,0)==0.0);
 // Looks (dlss5.8d): the default settings are Balanced; each look matches
 // itself after ApplyLook and leaves Pass Count, Resolution Scale and the
 // compare settings alone; anything else is Custom.
 Settings look{};CHECK(MatchingLook(look)==1);
 look.passes=3;look.scale=50.0f;look.compare=true;look.compareKey=0x70;
 for(size_t n=0;n<kLookCount;++n){ApplyLook(look,n);CHECK(MatchingLook(look)==n);CHECK(look.passes==3&&look.scale==50.0f&&look.compare&&look.compareKey==0x70);}
 // autoscale.2: Balanced is the provider's own default (Intensity, Structure, Tone 1, Faces & skin following
 // Structure), Subtle half its intensity, Strong further than Balanced.
 CHECK(kLooks[1].intensity==1.0f&&kLooks[1].model==0&&kLooks[1].structure==1.0f&&kLooks[1].tone==1.0f&&kLooks[1].character==-1.0f);
 CHECK(kLooks[0].intensity==0.5f&&kLooks[0].structure==kLooks[1].structure&&kLooks[0].tone==kLooks[1].tone&&kLooks[0].character==-1.0f);
 CHECK(kLooks[2].intensity==1.0f&&kLooks[2].structure>kLooks[1].structure&&kLooks[2].tone>kLooks[1].tone&&kLooks[2].structure<=kMaxStrength);
 CHECK(!Settings{}.scaleAuto&&Settings{}.scale==100.0f);
 look.tone=0.45f;CHECK(MatchingLook(look)==kLookCount);ApplyLook(look,kLookCount);CHECK(look.tone==0.45f);
 // Compare settings are sanitized: split within 5-95 %, no mouse buttons, Escape or Windows keys.
 Settings compare{};CHECK(!compare.compare&&compare.compareSplit==50.0f&&compare.compareKey==0);
 compare.compareSplit=200.0f;compare.compareKey=0x1B;CHECK(Sanitized(compare).compareSplit==kMaxCompareSplit&&Sanitized(compare).compareKey==0);
 compare.compareSplit=1.0f;compare.compareKey=0x01;CHECK(Sanitized(compare).compareSplit==kMinCompareSplit&&Sanitized(compare).compareKey==0);
 compare.compareKey=0x5B;CHECK(Sanitized(compare).compareKey==0);compare.compareKey=0x06;CHECK(Sanitized(compare).compareKey==0x06);
 compare.compareKey=0x70;CHECK(Sanitized(compare).compareKey==0x70);compare.compareKey=0xFF;CHECK(Sanitized(compare).compareKey==0);
 std::printf("DLSS5 auto checks=%u PASS\n",checks);
}
