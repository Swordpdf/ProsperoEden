from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1]
c=Path((r/'.local/headless-cache').read_text().strip())
t=(c/'native-local/headless/nso.cpp').read_text()
body=t[t.index('    const size_t module_start'):t.index('    // Apply patches if necessary')]
zero=t[t.index('    // Initialize BSS and page padding directly'):t.index('    // Load codeset for current process')]
code=r'''
#include <algorithm>
#include <cstdio>
#include <array>
#include <vector>
#include <string>
#include <optional>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <span>
#include <lz4.h>
using u8=uint8_t; using u32=uint32_t; using u32_le=u32; using u64=uint64_t;
constexpr u32 NSO_ARGUMENT_DATA_ALLOCATION_SIZE=4096;
struct NSOArgumentHeader {u32 allocated,size; char reserved[24];};
u32 PageAlignSize(u32 x) {return (x+4095)&~4095;}
namespace Settings { struct Arg {std::string value; const std::string& GetValue()const{return value;}}; struct {Arg program_args; bool dump_nso{};} values; }
struct PatchManager { bool patched{}; bool HasNSOPatch(int,const std::string&)const{return patched;} };
struct Memory {size_t address{},length{};bool fail{};bool ZeroBlock(size_t a,size_t n){address=a;length=n;return !fail;}} memory;
struct Process {Memory& GetMemory(){return memory;}} process;
// No ZBIC encoder here: a segment that does not decode must end the load.
namespace Common::Compression {int DecompressDataZBIC(std::span<u8>,std::span<const u8>){return 0;}}
namespace Common::Compression {int DecompressDataLZ4(u8* d,size_t n,const u8*s,size_t m){return LZ4_decompress_safe((const char*)s,(char*)d,m,n);}}
int constructions=0;
namespace Kernel {struct CodeSet { struct Segment {size_t addr{},offset{};u32 size{};}; std::vector<u8>memory;std::array<Segment,3>segments; CodeSet(){++constructions;} Segment&DataSegment(){return segments[2];}};}
struct Header {struct Segment {u32 offset,location,size,bss_size;};std::array<Segment,3>segments;std::array<u32,3>segments_compressed_size;u32 flags{};int build_id{};bool zbic{};bool IsZBICCompressed()const{return zbic;}bool IsSegmentCompressed(size_t i)const{return flags&(1u<<i);}};
struct File {std::vector<u8>bytes;bool short_read{};std::string GetName()const{return "test";}size_t GetSize()const{return bytes.size();}size_t Read(u8*d,size_t n,size_t o)const {if(short_read)return 0;assert(o+n<=bytes.size());memcpy(d,bytes.data()+o,n);return n;}};
std::vector<u8> image;
size_t staging_size{},staging_capacity{};
std::optional<u64> load(const Header&nso_header,const File&nso_file,bool load_into_process,bool should_pass_arguments=false,std::optional<PatchManager> pm={}) {
 const u64 load_base=0x100000;
'''+body+zero+r'''
 staging_size=codeset.memory.size();staging_capacity=codeset.memory.capacity();
 image=codeset.memory;
 if(image_size<1024*1024) {
   image.resize(image_size,0xCD); // Model dirty guest pages; zeroing must be explicit.
   if(!full_image) std::fill(image.begin()+staging_size,image.end(),0);
 }
 return load_base+image_size;
}
int main(){
 Header h{{{{0,0,4,0},{4,4096,4,0},{8,8192,4,6000}}},{{4,4,4}},0};
 File f{{1,2,3,4,5,6,7,8,9,10,11,12}};
 auto size=load(h,f,false);assert(size==0x104000 && constructions==0);
 assert(load(h,f,true)==size);assert(image.size()==16384);
 assert(staging_size==8196 && staging_capacity<16384);
 assert(memory.address==0x102004 && memory.length==16384-8196);
 assert(image[0]==1 && image[4096]==5 && image[8192]==9 && image.back()==0);
 auto reference=image;
 std::vector<u8> compressed(100); int n=LZ4_compress_default((const char*)f.bytes.data(),(char*)compressed.data(),4,100);assert(n>0);
 h.flags=1; h.segments[0].offset=f.bytes.size();h.segments_compressed_size[0]=n;
 f.bytes.insert(f.bytes.end(),compressed.begin(),compressed.begin()+n);
 assert(load(h,f,true)==size && image==reference);
 f.short_read=true;assert(!load(h,f,true));f.short_read=false;
 h.segments_compressed_size[0]=1;assert(!load(h,f,true));
 h.segments_compressed_size[0]=n;h.zbic=true;assert(!load(h,f,true));h.zbic=false;
 h.flags=0;h.segments[0].offset=0;h.segments_compressed_size[0]=4;
 h.segments[2].bss_size=1800u*1024*1024;int before=constructions;
 assert(load(h,f,false) && constructions==before); // No large allocation for layout.
 assert(load(h,f,true));assert(staging_capacity==8196 && memory.length>1800u*1024*1024);
 // Repeat after another module, with guest-memory clearing failure propagated.
 h.segments[2].bss_size=6000;assert(load(h,f,true));
 h.segments[2].bss_size=1800u*1024*1024;assert(load(h,f,true));assert(staging_capacity==8196);
 memory.fail=true;assert(!load(h,f,true));memory.fail=false;
 h.segments[2].bss_size=UINT32_MAX;assert(!load(h,f,false));
 h.segments[2].bss_size=6000;h.segments[0].offset=99999;assert(!load(h,f,true));
 h.segments[0].offset=0;h.segments[0].location=99999;assert(!load(h,f,true));
 h.segments[0].location=0;Settings::values.program_args.value="hello";
 assert(load(h,f,true,true)==0x105000);assert(image.size()==20480);
 assert(staging_size==8196+4096 && memory.address==0x103004);
 assert(std::string((char*)image.data()+8196+sizeof(NSOArgumentHeader),5)=="hello");
 memory.address=memory.length=0;
 assert(load(h,f,true,true,PatchManager{true})==0x105000);
 assert(staging_size==8196+4096 && memory.address==0x103004); // A patch works on the initialized image; BSS stays in guest memory.
 Settings::values.dump_nso=true;
 assert(load(h,f,true,true,PatchManager{})==0x105000 && staging_size==20480);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++20','-fsanitize=address,undefined','-g',str(p/'test.cpp'),'-I'+str(c/'source/.cache/cpm/lz4/ebb370ca83/lib'),str(c/'source/.cache/cpm/lz4/ebb370ca83/lib/lz4.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=20)
print('Actual NSO loader: bounded staging, repeated huge BSS, direct zero/failure, patch/dump compatibility, compressed/raw bytes, args, invalid input PASS')
