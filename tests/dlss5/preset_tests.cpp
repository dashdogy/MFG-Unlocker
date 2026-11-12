// dlss5presets.1: DLSS 5 preset codes and the saved presets file (dlss5_presets.h).
#include "dlss5_presets.h"
#include <cstdio>
#include <cstdlib>
unsigned checks=0;
#define CHECK(c) do{++checks;if(!(c)){std::fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#c);std::exit(1);}}while(0)
using namespace dlss5;
using namespace dlss5::presets;

// Every byte of a code's body: base32 back to bytes.
std::vector<uint8_t> Bytes(const std::string& code){std::vector<uint8_t> bytes;CHECK(FromBase32(std::string_view(code).substr(kPrefix.size()),bytes));return bytes;}
std::string Code(std::vector<uint8_t> bytes){
 const uint32_t crc=Crc32(bytes.data(),bytes.size());
 for(int shift=0;shift!=32;shift+=8)bytes.push_back(static_cast<uint8_t>(crc>>shift));
 return std::string(kPrefix)+Base32(bytes);
}

int main(){
 // Half floats: exact for the values the menu shows, nearest even otherwise, round trips stable.
 for(float value:{0.0f,1.0f,-1.0f,0.5f,1.5f,2.0f,0.18f,0.8f,100.0f,200.0f,500.0f,48.0f,0.05f,8.0f})
  CHECK(ToHalf(FromHalf(ToHalf(value)))==ToHalf(value));
 for(float value:{0.0f,1.0f,-1.0f,0.5f,1.5f,2.0f,100.0f,200.0f,48.0f,8.0f})CHECK(FromHalf(ToHalf(value))==value);
 CHECK(std::abs(FromHalf(ToHalf(0.18f))-0.18f)<0.0002f);CHECK(std::abs(FromHalf(ToHalf(203.37f))-203.37f)<0.2f);
 CHECK(ToHalf(1e-9f)==0);CHECK(FromHalf(0x0001)>0.0f&&FromHalf(0x0001)<1e-7f);CHECK(ToHalf(FromHalf(0x0001))==0x0001);
 CHECK(!std::isfinite(FromHalf(0x7C00)));CHECK(ToHalf(1e6f)==0x7C00);
 // CRC-32 check value.
 CHECK(Crc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xCBF43926u);
 // Base32 round trips; O, I, L and lower case read as the code meant; U and padding bits refuse.
 {
  const std::vector<uint8_t> bytes{0x00,0xFF,0x10,0x80,0x7F,0x01,0xAB};
  std::vector<uint8_t> back;CHECK(FromBase32(Base32(bytes),back)&&back==bytes);
  std::string text=Base32(bytes);for(char& c:text)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
  CHECK(FromBase32(text,back)&&back==bytes);
  CHECK(Base32Value('O')==0&&Base32Value('o')==0&&Base32Value('I')==1&&Base32Value('l')==1&&Base32Value('U')<0&&Base32Value(' ')<0);
  CHECK(!FromBase32("0U",back));
  // One byte is two characters with 2 padding bits, which must be zero.
  CHECK(FromBase32("Z0",back)&&back.size()==1&&back[0]==0xF8);CHECK(!FromBase32("Z1",back));
  // A third character leaves 7 bits: no code ends so.
  CHECK(!FromBase32("Z00",back));
 }
 // Names: control characters out, spaces trimmed, invalid UTF-8 dropped, cut on a character boundary.
 CHECK(CleanName("  Night\tCity \r\n")=="NightCity");
 CHECK(CleanName("\xC3\xA9t\xC3\xA9")=="\xC3\xA9t\xC3\xA9");CHECK(CleanName("a\xC3""b\xFF")=="ab");
 CHECK(CleanName("Look ##2 ###x")=="Look #2 #x");CHECK(CleanName("   ")=="");
 {
  std::string longName(kMaxName-1,'x');longName+="\xE4\xB8\xAD";
  CHECK(CleanName(longName)==std::string(kMaxName-1,'x'));
 }

 // Defaults are the empty preset: format, empty name, no records, checksum.
 const Settings defaults{};
 CHECK(Records(defaults).empty());
 const std::string empty=Encode(defaults,"");
 CHECK(empty.rfind("DLSS5-",0)==0);CHECK(Bytes(empty).size()==6);
 for(char c:empty.substr(kPrefix.size()))CHECK(Base32Value(c)>=0);
 {
  Preset preset;CHECK(Decode(empty,preset)==Error::eNone);
  CHECK(preset.name.empty()&&preset.code==empty&&preset.settings==defaults&&preset.skipped==0);
 }

 // A tuned preset round trips: every saved setting of the look, the name, and the code itself.
 Settings tuned{};
 tuned.enabled=false;tuned.stage=static_cast<uint32_t>(Stage::eRender);tuned.resources=2;tuned.uiCorrection=2;tuned.encoding=3;
 tuned.diffuseWhite=203.0f;tuned.diffuseWhiteOverride=true;tuned.intensity=0.8f;tuned.model=1;tuned.structure=1.5f;tuned.tone=0.4f;
 tuned.characterMask=false;tuned.character=0.75f;tuned.scale=85.0f;tuned.scaleAuto=true;tuned.domain=2;tuned.downsample=1;
 tuned.upsample=3;tuned.reconstruction=1;tuned.preset=1;tuned.passes=3;tuned.signal=1;tuned.blockHudless=false;tuned.exposure=2.5f;
 tuned.exposureMode=2;tuned.adaptiveExposure=true;tuned.boost=true;tuned.laterCustom=true;tuned.laterModel=2;tuned.laterIntensity=0.6f;
 tuned.laterStructure=0.9f;tuned.laterTone=0.2f;tuned.laterCharacter=-1.0f;tuned.blend=1.25f;tuned.color=0.5f;tuned.highlightGuard=true;
 tuned.highlightLimit=3.0f;tuned.haloDark=0.4f;tuned.haloBright=0.2f;tuned.haloRadius=3;tuned.supersample=1;tuned.knee=0.7f;
 tuned.adaptiveTarget=0.25f;tuned.boostTolerance=2;
 // Not part of a preset.
 tuned.compare=true;tuned.compareSplit=30.0f;tuned.compareKey=0x70;tuned.compareSwap=true;tuned.debugView=3;
 const std::string code=Encode(tuned,"Night City  ");
 CHECK(code.size()<200);
 Preset decoded;CHECK(Decode(code,decoded)==Error::eNone);
 CHECK(decoded.name=="Night City");CHECK(decoded.code==code);CHECK(decoded.skipped==0);
 CHECK(Records(decoded.settings)==Records(tuned));
 for(const char* key:kKeys){
  const settings_json::Field& field=*FieldFor(key);
  if(field.kind==settings_json::Kind::eBool)CHECK(decoded.settings.*field.flag==tuned.*field.flag);
  else if(field.kind==settings_json::Kind::eUnsigned)CHECK(decoded.settings.*field.integer==tuned.*field.integer);
  else CHECK(std::abs(decoded.settings.*field.real-tuned.*field.real)<=std::abs(tuned.*field.real)*0.001f+0.0005f);
 }
 // Compare and Inspect stay out of the code: the decoded preset has the defaults there.
 CHECK(!decoded.settings.compare&&decoded.settings.compareKey==0&&decoded.settings.debugView==0&&!decoded.settings.compareSwap);
 CHECK(Records([&]{Settings s=tuned;s.compare=false;s.compareKey=0;s.debugView=0;s.enabled=true;return s;}())==Records(tuned));

 // Apply: the preset's settings over the current ones, DLSS 5 on, Compare and Inspect kept, the result re-encodes to the
 // same records (so the menu shows it as that preset).
 {
  Settings current{};current.compare=true;current.compareSplit=70.0f;current.compareKey=0x71;current.debugView=1;current.intensity=0.1f;
  const Settings applied=Apply(current,decoded.settings);
  CHECK(applied.enabled);CHECK(applied.stage==static_cast<uint32_t>(Stage::eRender));
  CHECK(applied.compare&&applied.compareSplit==70.0f&&applied.compareKey==0x71&&applied.debugView==1);
  CHECK(Records(applied)==Records(decoded.settings));CHECK(Encode(applied,"Night City")==code);
  CHECK(applied==Sanitized(applied));
 }

 // Codes are found in chat text, in either case, with quotes or code blocks around them and hyphens inside.
 {
  Preset found;
  CHECK(Decode("Try my look: `"+code+"` enjoy :)",found)==Error::eNone&&found.code==code);
  std::string lower=code;for(char& c:lower)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
  CHECK(Decode("\""+lower+"\"",found)==Error::eNone&&found.code==code);
  std::string grouped=code.substr(0,kPrefix.size());
  for(size_t i=kPrefix.size();i<code.size();++i){grouped+=code[i];if((i-kPrefix.size())%4==3)grouped+='-';}
  CHECK(Decode(grouped,found)==Error::eNone&&found.code==code);
  // A first "DLSS5-" that is not a code does not hide the real one after it.
  CHECK(Decode("DLSS5-preset for Witcher: "+code,found)==Error::eNone&&found.code==code);
  CHECK(Decode("nothing here",found)==Error::eNoCode);CHECK(Decode("",found)==Error::eNoCode);
  CHECK(Decode("DLSS5-",found)==Error::eDamaged);
  // Cut short, one character changed, or a character outside the alphabet: refused.
  CHECK(Decode(code.substr(0,code.size()-3),found)==Error::eDamaged);
  for(size_t at:{kPrefix.size(),code.size()/2,code.size()-1}){
   std::string typo=code;typo[at]=typo[at]=='7'?'8':'7';
   CHECK(Decode(typo,found)==Error::eDamaged);
  }
 }

 // Format and records: a newer format is eNewer; unknown ids are skipped by their kind's length; a known id with the
 // wrong kind, a repeated id, a name over the limit or a record cut short is damaged.
 {
  Preset preset;
  CHECK(Decode(Code({2,0}),preset)==Error::eNewer);CHECK(Decode(Code({0,0}),preset)==Error::eDamaged);
  CHECK(Decode(Code({1,0,0x80|63,7,0xC0|62,1,2,0x40|61}),preset)==Error::eNone);
  CHECK(preset.skipped==3&&preset.settings.intensity==1.0f&&preset.settings.model==0);
  // id 6 (Intensity) as a byte: wrong kind.
  CHECK(Decode(Code({1,0,0x80|6,1}),preset)==Error::eDamaged);
  // id 7 (Style) as a half: wrong kind.
  CHECK(Decode(Code({1,0,0xC0|7,0,0x3C}),preset)==Error::eDamaged);
  CHECK(Decode(Code({1,0,0x80|7,1,0x80|7,2}),preset)==Error::eDamaged);
  CHECK(Decode(Code({1,static_cast<uint8_t>(kMaxName+1)}),preset)==Error::eDamaged);
  CHECK(Decode(Code({1,0,0xC0|6,0}),preset)==Error::eDamaged);
  // A name with a control character is not one Encode writes.
  CHECK(Decode(Code({1,2,'a','\n'}),preset)==Error::eDamaged);
  // Out-of-range values come back into range (a newer build's wider range), as Sanitized does for the saved file.
  CHECK(Decode(Code({1,0,0x80|19,200,0xC0|8,0x00,0x4C}),preset)==Error::eNone);
  CHECK(preset.settings.passes==kMaxPasses&&preset.settings.structure==kMaxStrength);
  // A NaN half is damaged.
  CHECK(Decode(Code({1,0,0xC0|6,0x01,0x7E}),preset)==Error::eDamaged);
  // A newer format next to a damaged code reports the newer one.
  CHECK(Decode(Code({2,0})+" "+code.substr(0,20),preset)==Error::eNewer);
 }

 // The saved presets file: comments and blank lines skipped, unreadable lines kept as written, CRLF or LF, a BOM.
 {
  const std::string other=Encode(Settings{},"Plain");
  const std::string content="\xEF\xBB\xBF# comment\r\n\r\n  "+code+"  \r\nDLSS5-NOTACODE\n"+other+"\n";
  std::vector<Entry> entries=ParseStore(content);
  CHECK(entries.size()==3);CHECK(entries[0].readable&&entries[0].preset.name=="Night City");
  CHECK(!entries[1].readable&&entries[1].raw=="DLSS5-NOTACODE");CHECK(entries[2].readable&&entries[2].preset.name=="Plain");
  CHECK(ReadableCount(entries)==2);
  const std::string written=WriteStore(entries);
  std::vector<Entry> again=ParseStore(written);
  CHECK(again.size()==3&&again[0].preset.code==code&&again[1].raw=="DLSS5-NOTACODE"&&again[2].preset.code==other);
  CHECK(WriteStore(again)==written);
  CHECK(FindName(entries,"night city")==0);CHECK(FindName(entries,"Plain")==2);CHECK(FindName(entries,"DLSS5-NOTACODE")==kNone);
  CHECK(FindSettings(entries,tuned)==0);CHECK(FindSettings(entries,Settings{})==2);
  Settings changed=tuned;changed.intensity=0.5f;CHECK(FindSettings(entries,changed)==kNone);
  // Compare and on/off are not part of a preset: still that preset.
  changed=tuned;changed.compare=!changed.compare;changed.enabled=!changed.enabled;CHECK(FindSettings(entries,changed)==0);
  CHECK(UniqueName(entries,"Fresh")=="Fresh");CHECK(UniqueName(entries,"night city")=="night city (2)");
  Entry second;second.readable=true;second.preset.name="Night City (2)";entries.push_back(second);
  CHECK(UniqueName(entries,"Night City")=="Night City (3)");
  const std::string longName(kMaxName,'n');Entry full;full.readable=true;full.preset.name=longName;entries.push_back(full);
  const std::string unique=UniqueName(entries,longName);CHECK(unique.size()<=kMaxName&&unique.rfind(" (2)")==unique.size()-4);
  CHECK(ParseStore("").empty());CHECK(ParseStore("# only\r\n").empty());
 }
 std::printf("dlss5 presets: %u checks passed\n",checks);
 return 0;
}
