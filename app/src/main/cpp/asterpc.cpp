#include <jni.h>
#include "winapi_dispatcher.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <utility>
#include <unordered_map>

namespace {
enum Reg : unsigned { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
                      R8, R9, R10, R11, R12, R13, R14, R15 };
constexpr size_t MEMORY_SIZE = 1024 * 1024;
constexpr uint64_t DEFAULT_STEP_LIMIT = 1000000;
constexpr uint64_t API_STUB_BASE = 0x0000700000000000ULL;

struct Cpu {
    std::array<uint64_t,16> r{};
    uint64_t rip=0, steps=0;
    bool zf=false,sf=false,cf=false,of=false, halted=false;
    std::string error;
};

struct Operand {
    bool isReg=false;
    unsigned reg=0;
    uint64_t address=0;
};

struct Machine {
    Cpu cpu;
    std::vector<uint8_t> mem;
    uint64_t memoryBase=0;
    asterpc::WinApiDispatcher apiDispatcher;
    asterpc::Win64CallFrame apiFrame;
    std::unordered_map<uint64_t,std::pair<std::string,std::string>> apiStubs;
    explicit Machine(const std::vector<uint8_t>& program,
                     uint64_t baseAddress=0,uint64_t entryAddress=0)
        : mem(std::max<size_t>(MEMORY_SIZE,program.size()+64u*1024u)),
          memoryBase(baseAddress) {
        if(program.size()>128u*1024u*1024u ||
           memoryBase>UINT64_MAX-mem.size()) {
            cpu.error="guest image exceeds the bounded CPU memory model";
            return;
        }
        std::copy(program.begin(),program.end(),mem.begin());
        cpu.rip=entryAddress?entryAddress:memoryBase;
        cpu.r[RSP]=memoryBase+mem.size()-16;
    }
    bool bindApiStub(uint64_t address,const std::string& dll,const std::string& symbol) {
        if(address<API_STUB_BASE || address-API_STUB_BASE>0x100000ULL ||
           apiStubs.find(address)!=apiStubs.end()) return false;
        apiStubs.emplace(address,std::make_pair(dll,symbol));
        return true;
    }
    bool invokeApiStub(uint64_t address) {
        const auto found=apiStubs.find(address);
        if(found==apiStubs.end()) return fail("unresolved external API thunk");
        apiFrame.rcx=cpu.r[RCX];
        apiFrame.rdx=cpu.r[RDX];
        apiFrame.r8=cpu.r[R8];
        apiFrame.r9=cpu.r[R9];
        const auto result=apiDispatcher.dispatch(found->second.first,found->second.second,apiFrame);
        if(result.status==asterpc::DispatchStatus::NotFound)
            return fail("Windows API dispatcher rejected imported symbol");
        cpu.r[RAX]=result.value;
        if(result.status==asterpc::DispatchStatus::ExitRequested || apiFrame.exitRequested)
            cpu.halted=true;
        return true;
    }
    bool fail(const char* why) { cpu.error=why; return false; }
    bool range(uint64_t a,size_t n) const {
        if(a<memoryBase) return false;
        const uint64_t offset=a-memoryBase;
        return offset<=mem.size() && n<=mem.size()-static_cast<size_t>(offset);
    }
    bool fetch8(uint8_t& v) {
        if(!range(cpu.rip,1)) return fail("instruction fetch outside guest memory");
        v=mem[static_cast<size_t>(cpu.rip-memoryBase)];
        ++cpu.rip;
        return true;
    }
    bool fetchN(uint64_t& v,unsigned n) {
        if(!range(cpu.rip,n)) return fail("truncated instruction immediate");
        const size_t offset=static_cast<size_t>(cpu.rip-memoryBase);
        v=0; for(unsigned i=0;i<n;i++) v|=uint64_t(mem[offset+i])<<(i*8);
        cpu.rip+=n;
        return true;
    }
    bool readMem(uint64_t a,unsigned n,uint64_t& v) {
        if(!range(a,n)) return fail("guest memory read out of bounds");
        const size_t offset=static_cast<size_t>(a-memoryBase);
        v=0; for(unsigned i=0;i<n;i++) v|=uint64_t(mem[offset+i])<<(i*8);
        return true;
    }
    bool writeMem(uint64_t a,unsigned n,uint64_t v) {
        if(!range(a,n)) return fail("guest memory write out of bounds");
        const size_t offset=static_cast<size_t>(a-memoryBase);
        for(unsigned i=0;i<n;i++) mem[offset+i]=uint8_t(v>>(i*8));
        return true;
    }
    uint64_t readOp(const Operand& o,unsigned n) {
        if(o.isReg) return cpu.r[o.reg];
        uint64_t v=0; if(!readMem(o.address,n,v)) return 0; return v;
    }
    bool writeOp(const Operand& o,unsigned n,uint64_t v) {
        if(o.isReg) { cpu.r[o.reg]=v; return true; }
        return writeMem(o.address,n,v);
    }
    void setLogic(uint64_t v) { cpu.zf=(v==0); cpu.sf=(v>>63)!=0; cpu.cf=false; cpu.of=false; }
    void doAdd(uint64_t& a,uint64_t b) {
        uint64_t old=a; a+=b; cpu.zf=(a==0); cpu.sf=(a>>63)!=0;
        cpu.cf=(a<old); cpu.of=((~(old^b)&(old^a))>>63)!=0;
    }
    void doSub(uint64_t& a,uint64_t b) {
        uint64_t old=a; a-=b; cpu.zf=(a==0); cpu.sf=(a>>63)!=0;
        cpu.cf=(old<b); cpu.of=(((old^b)&(old^a))>>63)!=0;
    }
    bool decodeRM(uint8_t modrm,uint8_t rex,Operand& rm,unsigned& reg) {
        unsigned mod=modrm>>6, rawReg=(modrm>>3)&7, rawRm=modrm&7;
        reg=rawReg+((rex&4)?8:0);
        if(mod==3) { rm={true,rawRm+((rex&1)?8:0),0}; return true; }
        uint64_t base=0; bool hasBase=true; bool ripRelative=false;
        if(rawRm==4) {
            uint8_t sib; if(!fetch8(sib)) return false;
            unsigned scale=sib>>6, idx=((sib>>3)&7)+((rex&2)?8:0), b=(sib&7)+((rex&1)?8:0);
            if(((sib>>3)&7)!=4 || (rex&2)) base+=cpu.r[idx] << scale;
            if(mod==0 && (sib&7)==5) hasBase=false; else base+=cpu.r[b];
        } else if(mod==0 && rawRm==5) {
            hasBase=false;
            ripRelative=true;
        } else base=cpu.r[rawRm+((rex&1)?8:0)];
        int64_t disp=0;
        if(mod==0 && !hasBase) {
            uint64_t d; if(!fetchN(d,4)) return false;
            int32_t x; std::memcpy(&x,&d,4); disp=x;
            // In 64-bit mode, ModRM mod=00 r/m=101 is RIP-relative.
            // SIB base=101 remains a no-base absolute displacement form.
            if(ripRelative) base=cpu.rip;
        }
        else if(mod==1) { uint8_t d; if(!fetch8(d)) return false; disp=static_cast<int8_t>(d); }
        else if(mod==2) { uint64_t d; if(!fetchN(d,4)) return false; int32_t x; std::memcpy(&x,&d,4); disp=x; }
        int64_t addr=static_cast<int64_t>(base)+disp;
        if(addr<0) return fail("negative guest memory address");
        rm={false,0,static_cast<uint64_t>(addr)}; return true;
    }
    bool push(uint64_t v) {
        if(cpu.r[RSP]<8) return fail("stack underflow");
        cpu.r[RSP]-=8; return writeMem(cpu.r[RSP],8,v);
    }
    bool pop(uint64_t& v) {
        if(!readMem(cpu.r[RSP],8,v)) return false;
        cpu.r[RSP]+=8; return true;
    }
    bool branch(int64_t rel) {
        int64_t dest=static_cast<int64_t>(cpu.rip)+rel;
        if(dest<0 || static_cast<uint64_t>(dest)<memoryBase ||
           static_cast<uint64_t>(dest)-memoryBase>=mem.size())
            return fail("branch target outside guest memory");
        cpu.rip=static_cast<uint64_t>(dest); return true;
    }
    bool run(uint64_t limit=DEFAULT_STEP_LIMIT) {
        while(!cpu.halted && cpu.steps<limit) {
            uint8_t op; if(!fetch8(op)) return false; cpu.steps++;
            if(op==0x90) continue;
            if(op==0xF4) { cpu.halted=true; break; }
            uint8_t rex=0;
            if(op>=0x40 && op<=0x4F) { rex=op; if(!fetch8(op)) return false; }
            bool w=(rex&8)!=0;
            unsigned width=w?8:4;
            if(op>=0xB8 && op<=0xBF) {
                uint64_t imm; if(!fetchN(imm,w?8:4)) return false;
                unsigned dst=(op-0xB8)+((rex&1)?8:0);
                cpu.r[dst]=w?imm:uint32_t(imm); continue;
            }
            if(op>=0x50 && op<=0x57) { unsigned r=(op-0x50)+((rex&1)?8:0); if(!push(cpu.r[r])) return false; continue; }
            if(op>=0x58 && op<=0x5F) { uint64_t v; if(!pop(v)) return false; cpu.r[(op-0x58)+((rex&1)?8:0)]=v; continue; }
            if(op==0x68 || op==0x6A) {
                uint64_t v; if(!fetchN(v,op==0x68?4:1)) return false;
                if(op==0x68) { int32_t x; uint32_t t=uint32_t(v); std::memcpy(&x,&t,4); v=static_cast<uint64_t>(static_cast<int64_t>(x)); }
                else v=static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v)));
                if(!push(v)) return false; continue;
            }
            if(op==0xEB || op==0x74 || op==0x75 || op==0xE8 || op==0xE9) {
                int64_t rel;
                if(op==0xEB || op==0x74 || op==0x75) { uint8_t v; if(!fetch8(v)) return false; rel=static_cast<int8_t>(v); }
                else { uint64_t v; if(!fetchN(v,4)) return false; int32_t x; uint32_t t=uint32_t(v); std::memcpy(&x,&t,4); rel=x; }
                bool take=(op==0xEB||op==0xE8||op==0xE9)||(op==0x74&&cpu.zf)||(op==0x75&&!cpu.zf);
                if(take) { uint64_t ret=cpu.rip; if(op==0xE8&&!push(ret)) return false; if(!branch(rel)) return false; }
                continue;
            }
            if(op==0xC3) { uint64_t dest; if(!pop(dest)) return false; if(dest<memoryBase || dest-memoryBase>=mem.size()) return fail("RET target outside guest memory"); cpu.rip=dest; continue; }

            if(op==0x89||op==0x8B||op==0x01||op==0x03||op==0x29||op==0x2B||
               op==0x31||op==0x33||op==0x39||op==0x3B||op==0x85||op==0x8D) {
                uint8_t m; if(!fetch8(m)) return false;
                Operand rm; unsigned reg; if(!decodeRM(m,rex,rm,reg)) return false;
                if(op==0x8D) { if(rm.isReg) return fail("LEA requires a memory addressing form"); cpu.r[reg]=rm.address; continue; }
                uint64_t a=readOp(rm,width), b=cpu.r[reg], t; if(!cpu.error.empty()) return false;
                if(op==0x89) { if(!writeOp(rm,width,b)) return false; }
                else if(op==0x8B) cpu.r[reg]=a;
                else if(op==0x01) { t=a; doAdd(t,b); if(!writeOp(rm,width,t)) return false; }
                else if(op==0x03) { t=b; doAdd(t,a); cpu.r[reg]=t; }
                else if(op==0x29) { t=a; doSub(t,b); if(!writeOp(rm,width,t)) return false; }
                else if(op==0x2B) { t=b; doSub(t,a); cpu.r[reg]=t; }
                else if(op==0x31) { t=a^b; setLogic(t); if(!writeOp(rm,width,t)) return false; }
                else if(op==0x33) { t=b^a; setLogic(t); cpu.r[reg]=t; }
                else if(op==0x39) { t=a; doSub(t,b); }
                else if(op==0x3B) { t=b; doSub(t,a); }
                else { setLogic(a&b); }
                continue;
            }
            if(op==0xC7 || op==0x81 || op==0x83 || op==0xF7 || op==0xFF ||
               op==0xC1 || op==0xD1 || op==0xD3) {
                uint8_t m; if(!fetch8(m)) return false;
                Operand rm; unsigned reg; if(!decodeRM(m,rex,rm,reg)) return false;
                unsigned group=(m>>3)&7;
                if(op==0xC7) {
                    if(group!=0) return fail("unsupported C7 group");
                    uint64_t v; if(!fetchN(v,4)) return false;
                    int32_t x; uint32_t q=uint32_t(v); std::memcpy(&x,&q,4);
                    if(!writeOp(rm,8,static_cast<uint64_t>(static_cast<int64_t>(x)))) return false;
                    continue;
                }
                if(op==0xF7 && group==0) {
                    uint64_t imm; if(!fetchN(imm,4)) return false;
                    uint64_t x=readOp(rm,8); if(!cpu.error.empty()) return false; setLogic(x & static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(imm)))); continue;
                }
                if(op==0xFF && group==2) {
                    const uint64_t target=readOp(rm,8);
                    if(!cpu.error.empty()) return false;
                    if(apiStubs.find(target)!=apiStubs.end()) {
                        if(!invokeApiStub(target)) return false;
                        continue;
                    }
                    if(target<memoryBase || target-memoryBase>=mem.size())
                        return fail("indirect CALL target is neither guest code nor a registered API thunk");
                    const uint64_t returnAddress=cpu.rip;
                    if(!push(returnAddress)) return false;
                    cpu.rip=target;
                    continue;
                }
                if(op==0xFF && (group==0||group==1)) {
                    uint64_t x=readOp(rm,8), old=x; if(!cpu.error.empty()) return false;
                    if(group==0) doAdd(x,1); else doSub(x,1);
                    cpu.cf=(group==0)?(old==UINT64_MAX):(old==0);
                    if(!writeOp(rm,8,x)) return false; continue;
                }
                if(op==0xF7 && group==2) { uint64_t x=~readOp(rm,8); if(!writeOp(rm,8,x)) return false; continue; }
                if(op==0xF7 && group==3) { uint64_t x=readOp(rm,8); if(!cpu.error.empty()) return false; doSub(x,0); x=0-x; if(!writeOp(rm,8,x)) return false; continue; }
                uint64_t imm=0;
                if(op==0x81) { if(!fetchN(imm,4)) return false; int32_t x; uint32_t q=uint32_t(imm); std::memcpy(&x,&q,4); imm=static_cast<uint64_t>(static_cast<int64_t>(x)); }
                else if(op==0x83) { uint8_t q; if(!fetch8(q)) return false; imm=static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(q))); }
                else if(op==0xC1) { uint8_t q; if(!fetch8(q)) return false; imm=q; }
                else if(op==0xD1) imm=1;
                else if(op==0xD3) imm=cpu.r[RCX]&0xFF;
                else return fail("unsupported unary or group instruction");
                uint64_t x=readOp(rm,8), result=x; if(!cpu.error.empty()) return false;
                if(op==0xC1||op==0xD1||op==0xD3) {
                    unsigned count=static_cast<unsigned>(imm)&63;
                    if(group==4||group==6) result=x<<count;
                    else if(group==5) result=x>>count;
                    else if(group==7) result=static_cast<uint64_t>(static_cast<int64_t>(x)>>count);
                    else return fail("unsupported shift group");
                    if(count) { cpu.cf=group==4||group==6 ? ((x>>(64-count))&1) : ((x>>(count-1))&1); setLogic(result); }
                    if(!writeOp(rm,8,result)) return false; continue;
                }
                if(group==0) doAdd(result,imm);
                else if(group==1) { result|=imm; setLogic(result); }
                else if(group==4) { result&=imm; setLogic(result); }
                else if(group==5) doSub(result,imm);
                else if(group==6) { result^=imm; setLogic(result); }
                else if(group==7) { doSub(result,imm); continue; }
                else return fail("unsupported immediate operation");
                if(!writeOp(rm,8,result)) return false; continue;
            }
            return fail("unsupported opcode");
        }
        if(!cpu.halted && cpu.steps>=limit) return fail("instruction execution limit reached");
        return true;
    }
};

static std::string test(const std::vector<uint8_t>& p,uint64_t expected) {
    Machine m(p); bool ok=m.run(); std::ostringstream s;
    s<<(ok&&m.cpu.r[RAX]==expected?"PASS":"FAIL")<<" RAX="<<m.cpu.r[RAX]<<" steps="<<m.cpu.steps;
    if(!ok) s<<" error="<<m.cpu.error; else if(m.cpu.r[RAX]!=expected) s<<" expected="<<expected;
    return s.str();
}
static // Minimal guest virtual-address-space model. Regions are isolated and permission checked.
// This is metadata-backed RAM for the prototype, not Windows paging or an MMU.
class GuestAddressSpace {
    struct Region {
        uint64_t base;
        std::vector<uint8_t> bytes;
        bool readable, writable, executable;
    };
    std::vector<Region> regions;
    size_t committed=0;
    static constexpr size_t MAX_COMMITTED=256u*1024u*1024u;
    const Region* locate(uint64_t address,size_t count) const {
        for(const auto& r:regions) {
            if(address<r.base) continue;
            const uint64_t offset=address-r.base;
            if(offset<=r.bytes.size() && count<=r.bytes.size()-static_cast<size_t>(offset))
                return &r;
        }
        return nullptr;
    }
    Region* locate(uint64_t address,size_t count) {
        for(auto& r:regions) {
            if(address<r.base) continue;
            const uint64_t offset=address-r.base;
            if(offset<=r.bytes.size() && count<=r.bytes.size()-static_cast<size_t>(offset))
                return &r;
        }
        return nullptr;
    }
public:
    bool map(uint64_t base,size_t size,bool read,bool write,bool execute) {
        if(!size || size>MAX_COMMITTED-committed || base>UINT64_MAX-size) return false;
        const uint64_t end=base+size;
        for(const auto& r:regions) {
            const uint64_t otherEnd=r.base+r.bytes.size();
            if(base<otherEnd && r.base<end) return false;
        }
        Region region{base,std::vector<uint8_t>(size,0),read,write,execute};
        regions.push_back(std::move(region));
        committed+=size;
        return true;
    }
    bool read8(uint64_t address,uint8_t& value) const {
        const Region* r=locate(address,1);
        if(!r||!r->readable) return false;
        value=r->bytes[static_cast<size_t>(address-r->base)];
        return true;
    }
    bool write8(uint64_t address,uint8_t value) {
        Region* r=locate(address,1);
        if(!r||!r->writable) return false;
        r->bytes[static_cast<size_t>(address-r->base)]=value;
        return true;
    }
    bool canFetch(uint64_t address) const {
        const Region* r=locate(address,1);
        return r&&r->readable&&r->executable;
    }
    bool mapBytes(uint64_t base,const std::vector<uint8_t>& source,size_t sourceOffset,
                  size_t size,bool read,bool write,bool execute) {
        if(sourceOffset>source.size() || size>source.size()-sourceOffset ||
           !size || size>MAX_COMMITTED-committed || base>UINT64_MAX-size) return false;
        const uint64_t end=base+size;
        for(const auto& r:regions) {
            const uint64_t otherEnd=r.base+r.bytes.size();
            if(base<otherEnd && r.base<end) return false;
        }
        Region region{base,std::vector<uint8_t>(source.begin()+static_cast<std::ptrdiff_t>(sourceOffset),
                                                source.begin()+static_cast<std::ptrdiff_t>(sourceOffset+size)),
                      read,write,execute};
        regions.push_back(std::move(region));
        committed+=size;
        return true;
    }
    size_t committedBytes() const { return committed; }
    size_t regionCount() const { return regions.size(); }
};

struct GuestModuleRecord {
    std::string name;
    std::string normalized;
    uint64_t diagnosticHandle=0;
    uint32_t imports=0;
    uint32_t recognized=0;
};

class GuestModuleRegistry {
    std::vector<GuestModuleRecord> modules;
    static std::string normalize(const std::string& name) {
        std::string result=name;
        std::transform(result.begin(),result.end(),result.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return result;
    }
public:
    bool registerImportModule(const std::string& name) {
        const std::string key=normalize(name);
        for(const auto& module:modules) if(module.normalized==key) return true;
        if(name.empty()||name.size()>260||modules.size()>=128) return false;
        const uint64_t handle=0x0000600000000000ULL+uint64_t(modules.size())*0x10000ULL;
        modules.push_back({name,key,handle,0,0});
        return true;
    }
    bool noteImport(const std::string& name,bool known) {
        const std::string key=normalize(name);
        for(auto& module:modules) if(module.normalized==key) {
            ++module.imports;
            if(known) ++module.recognized;
            return true;
        }
        return false;
    }
    size_t size() const { return modules.size(); }
    uint32_t importCount() const {
        uint32_t total=0; for(const auto& module:modules) total+=module.imports; return total;
    }
    std::string describe() const {
        std::ostringstream out;
        for(const auto& module:modules)
            out<<"  "<<module.name<<" handle=0x"<<std::hex<<module.diagnosticHandle<<std::dec
               <<" imports="<<module.imports<<" recognized="<<module.recognized<<" (metadata only)\n";
        return out.str();
    }
};

static std::string guestMemorySelfTest() {
    GuestAddressSpace space;
    uint8_t value=0;
    if(!space.map(0x10000000,4096,true,true,false)) return "FAIL: map writable data region";
    if(!space.write8(0x10000010,0x5a)||!space.read8(0x10000010,value)||value!=0x5a)
        return "FAIL: guest read/write";
    if(space.map(0x10000800,4096,true,true,false)) return "FAIL: overlapping region accepted";
    if(space.write8(0x20000000,1)) return "FAIL: unmapped write accepted";
    if(space.canFetch(0x10000010)) return "FAIL: data region executable";
    if(!space.map(0x20000000,4096,true,false,true)) return "FAIL: map read-only code region";
    if(space.write8(0x20000000,1)) return "FAIL: write to read-only code accepted";
    if(!space.canFetch(0x20000000)) return "FAIL: executable region rejected";
    GuestModuleRegistry modules;
    if(!modules.registerImportModule("KERNEL32.DLL") ||
       !modules.registerImportModule("kernel32.dll") || modules.size()!=1)
        return "FAIL: module registry case-insensitive deduplication";
    if(!modules.noteImport("Kernel32.dll",true) ||
       !modules.noteImport("kernel32.dll",false) || modules.importCount()!=2)
        return "FAIL: module import accounting";
    return "PASS: region mapping, overlap guard, permissions, bounds, and DLL registry";
}

struct PeImportBinding {
    std::string dll;
    std::string symbol;
    uint64_t iatAddress=0;
    uint64_t stubAddress=0;
    bool supported=false;
};

struct PeRuntimeImage {
    std::vector<uint8_t> bytes;
    uint64_t loadBase=0;
    uint32_t entryRva=0;
    std::vector<PeImportBinding> imports;
};

static std::string mapPeImage(const std::vector<uint8_t>& b,PeRuntimeImage* runtime=nullptr);

std::string selfTests() {
    const std::vector<uint8_t> basic={0x48,0xB8,42,0,0,0,0,0,0,0,0x48,0x83,0xC0,8,0xF4};
    const std::vector<uint8_t> memtest={0x48,0xB8,0x00,0x01,0x00,0x00,0,0,0,0,0x48,0xC7,0x00,0x2A,0,0,0,0x48,0x8B,0x08,0x48,0x89,0xC8,0xF4};
    const std::vector<uint8_t> arithmetic={0x48,0xB8,9,0,0,0,0,0,0,0,0x48,0xB9,3,0,0,0,0,0,0,0,0x48,0x01,0xC8,0x48,0x83,0xE8,1,0xF4};
    const std::vector<uint8_t> shifts={0x48,0xB8,3,0,0,0,0,0,0,0,0x48,0xC1,0xE0,2,0xF4};
    std::string a=test(basic,50),b=test(memtest,42),c=test(arithmetic,11),d=test(shifts,12);
    const std::string vm=guestMemorySelfTest();

    // End-to-end interpreter -> indirect CALL -> Win64 API thunk -> RAX.
    std::vector<uint8_t> apiProgram={0x48,0xB8};
    auto append64=[&](uint64_t value) {
        for(unsigned n=0;n<8;++n) apiProgram.push_back(uint8_t(value>>(n*8)));
    };
    append64(API_STUB_BASE+0x10);
    apiProgram.insert(apiProgram.end(),{0xFF,0xD0,0xF4});
    Machine apiMachine(apiProgram);
    const bool apiBound=apiMachine.bindApiStub(API_STUB_BASE+0x10,"kernel32.dll","GetTickCount64");
    apiMachine.apiFrame.virtualMilliseconds=0x100000005ULL;
    const bool apiCallPass=apiBound && apiMachine.run() &&
                           apiMachine.cpu.r[RAX]==0x100000005ULL;

    // Real x64 IAT-style indirection: CALL qword ptr [RIP+disp32].
    std::vector<uint8_t> iatProgram={0xFF,0x15,0xFA,0x00,0x00,0x00,0xF4};
    Machine iatMachine(iatProgram);
    const uint64_t iatTarget=API_STUB_BASE+0x50;
    for(unsigned n=0;n<8;++n)
        iatMachine.mem[0x100+n]=uint8_t(iatTarget>>(n*8));
    const bool iatBound=iatMachine.bindApiStub(iatTarget,"kernel32.dll","GetTickCount64");
    iatMachine.apiFrame.virtualMilliseconds=0xABCDEF0123ULL;
    const bool iatCallPass=iatBound && iatMachine.run() &&
                           iatMachine.cpu.r[RAX]==0xABCDEF0123ULL;

    std::vector<uint8_t> lastErrorProgram={0x48,0xB9};
    for(unsigned n=0;n<8;++n) lastErrorProgram.push_back(uint8_t(uint64_t(1234)>>(n*8)));
    lastErrorProgram.insert(lastErrorProgram.end(),{0x48,0xB8});
    for(unsigned n=0;n<8;++n) lastErrorProgram.push_back(uint8_t((API_STUB_BASE+0x20)>>(n*8)));
    lastErrorProgram.insert(lastErrorProgram.end(),{0xFF,0xD0,0x48,0xB8});
    for(unsigned n=0;n<8;++n) lastErrorProgram.push_back(uint8_t((API_STUB_BASE+0x30)>>(n*8)));
    lastErrorProgram.insert(lastErrorProgram.end(),{0xFF,0xD0,0xF4});
    Machine lastErrorMachine(lastErrorProgram);
    const bool lastErrorBound=lastErrorMachine.bindApiStub(API_STUB_BASE+0x20,"kernel32.dll","SetLastError") &&
                              lastErrorMachine.bindApiStub(API_STUB_BASE+0x30,"kernel32.dll","GetLastError");
    const bool lastErrorCallPass=lastErrorBound && lastErrorMachine.run() &&
                                 lastErrorMachine.cpu.r[RAX]==1234;

    std::vector<uint8_t> exitProgram={0x48,0xB9,42,0,0,0,0,0,0,0,0x48,0xB8};
    for(unsigned n=0;n<8;++n) exitProgram.push_back(uint8_t((API_STUB_BASE+0x40)>>(n*8)));
    exitProgram.insert(exitProgram.end(),{0xFF,0xD0});
    Machine exitMachine(exitProgram);
    const bool exitBound=exitMachine.bindApiStub(API_STUB_BASE+0x40,"kernel32.dll","ExitProcess");
    const bool exitCallPass=exitBound && exitMachine.run() && exitMachine.cpu.halted &&
                            exitMachine.apiFrame.exitRequested && exitMachine.apiFrame.exitCode==42;

    // Build a tiny synthetic PE32+ image to exercise the actual section mapper.
    std::vector<uint8_t> peFixture(0x400,0);
    auto put16=[&](size_t o,uint16_t v) {
        peFixture[o]=uint8_t(v); peFixture[o+1]=uint8_t(v>>8);
    };
    auto put32=[&](size_t o,uint32_t v) {
        for(unsigned n=0;n<4;++n) peFixture[o+n]=uint8_t(v>>(n*8));
    };
    auto put64=[&](size_t o,uint64_t v) {
        for(unsigned n=0;n<8;++n) peFixture[o+n]=uint8_t(v>>(n*8));
    };
    peFixture[0]='M'; peFixture[1]='Z'; put32(0x3c,0x80);
    peFixture[0x80]='P'; peFixture[0x81]='E';
    put16(0x84,0x8664); put16(0x86,1); put16(0x94,112);
    put16(0x98,0x20b); put32(0xA8,0x1000);
    put64(0xB0,0x10000000ULL); put32(0xB8,0x1000); put32(0xBC,0x200);
    put32(0xD0,0x2000); put32(0xD4,0x200); put32(0x104,0);
    peFixture[0x108]='.'; peFixture[0x109]='t'; peFixture[0x10A]='e';
    peFixture[0x10B]='x'; peFixture[0x10C]='t';
    put32(0x110,0x100); put32(0x114,0x1000);
    put32(0x118,0x200); put32(0x11C,0x200);
    put32(0x12C,0x60000020);
    peFixture[0x200]=0x90; peFixture[0x201]=0xC3;
    const std::string peMap=mapPeImage(peFixture);
    const bool peMapPass=peMap.rfind("PE32+ IMAGE MAPPED INTO GUEST ADDRESS SPACE",0)==0 &&
                         peMap.find("Guest memory regions: 2")!=std::string::npos &&
                         peMap.find("Executable sections: 1")!=std::string::npos;
    std::vector<uint8_t> overlappingFixture=peFixture;
    overlappingFixture[0x114]=0x00; overlappingFixture[0x115]=0x01;
    overlappingFixture[0x116]=0x00; overlappingFixture[0x117]=0x00;
    const std::string overlapResult=mapPeImage(overlappingFixture);
    const bool overlapRejected=overlapResult.find("overlapping PE sections")!=std::string::npos;

    // Synthetic PE32+ imports: one implemented Win32 API and one recognized-but-unimplemented API.
    std::vector<uint8_t> importFixture(0x600,0);
    auto i16=[&](size_t o,uint16_t v){importFixture[o]=uint8_t(v);importFixture[o+1]=uint8_t(v>>8);};
    auto i32=[&](size_t o,uint32_t v){for(unsigned n=0;n<4;++n)importFixture[o+n]=uint8_t(v>>(n*8));};
    auto i64=[&](size_t o,uint64_t v){for(unsigned n=0;n<8;++n)importFixture[o+n]=uint8_t(v>>(n*8));};
    importFixture[0]='M'; importFixture[1]='Z'; i32(0x3c,0x80);
    importFixture[0x80]='P'; importFixture[0x81]='E';
    i16(0x84,0x8664); i16(0x86,1); i16(0x94,128);
    i16(0x98,0x20b); i32(0xA8,0x1200); i64(0xB0,0x10000000ULL);
    i32(0xB8,0x1000); i32(0xBC,0x200); i32(0xD0,0x2000);
    i32(0xD4,0x200); i32(0x104,2);
    i32(0x110,0x1100); i32(0x114,40);
    // Section table begins at 0x118; its raw data maps file 0x200 to RVA 0x1000.
    importFixture[0x118]='.'; importFixture[0x119]='t'; importFixture[0x11A]='e';
    importFixture[0x11B]='x'; importFixture[0x11C]='t';
    i32(0x120,0x800); i32(0x124,0x1000); i32(0x128,0x400);
    i32(0x12C,0x200); i32(0x13C,0x60000020);
    // Import descriptor: OriginalFirstThunk=0x1140, DLL name=0x1180, IAT=0x1160.
    i32(0x300,0x1140); i32(0x30C,0x1180); i32(0x310,0x1160);
    i64(0x340,0x11A0); i64(0x348,0x11B0); i64(0x350,0);
    i64(0x360,0x11A0); i64(0x368,0x11B0); i64(0x370,0);
    const char* dllName="KERNEL32.dll";
    std::memcpy(importFixture.data()+0x380,dllName,std::strlen(dllName)+1);
    i16(0x3A0,0); std::memcpy(importFixture.data()+0x3A2,"GetTickCount64",15);
    i16(0x3B0,0); std::memcpy(importFixture.data()+0x3B2,"GetModuleHandleA",17);
    // Entry RVA 0x1200 calls through the IAT at RVA 0x1160, then halts.
    const uint8_t peRuntimeCode[]={0xFF,0x15,0x5A,0xFF,0xFF,0xFF,0xF4};
    std::memcpy(importFixture.data()+0x400,peRuntimeCode,sizeof(peRuntimeCode));
    PeRuntimeImage runtimeImage;
    const std::string importMap=mapPeImage(importFixture,&runtimeImage);
    const bool importResolverPass=
        importMap.find("Callable Win32 API thunks (subset): 1")!=std::string::npos &&
        importMap.find("Unresolved imports: 1")!=std::string::npos &&
        importMap.find("GetTickCount64 -> callable Win32 API thunk")!=std::string::npos &&
        importMap.find("GetModuleHandleA -> unresolved non-callable placeholder")!=std::string::npos &&
        runtimeImage.imports.size()==2 && runtimeImage.entryRva==0x1200;

    Machine peRuntimeMachine(runtimeImage.bytes,runtimeImage.loadBase,
                             runtimeImage.loadBase+runtimeImage.entryRva);
    bool peBindingsReady=importResolverPass;
    for(const auto& binding:runtimeImage.imports) {
        if(binding.supported)
            peBindingsReady=peRuntimeMachine.bindApiStub(binding.stubAddress,
                                                         binding.dll,binding.symbol) && peBindingsReady;
    }
    constexpr uint64_t PE_TEST_TICKS=0x123456789ABCULL;
    peRuntimeMachine.apiFrame.virtualMilliseconds=PE_TEST_TICKS;
    const bool peRuntimeCallPass=peBindingsReady && peRuntimeMachine.run() &&
                                 peRuntimeMachine.cpu.r[RAX]==PE_TEST_TICKS;

    std::vector<uint8_t> unsupportedPeBytes=runtimeImage.bytes;
    const uint8_t unsupportedCall[]={0xFF,0x15,0x62,0xFF,0xFF,0xFF,0xF4};
    if(unsupportedPeBytes.size()>=0x1200+sizeof(unsupportedCall))
        std::memcpy(unsupportedPeBytes.data()+0x1200,unsupportedCall,sizeof(unsupportedCall));
    Machine unsupportedPeMachine(unsupportedPeBytes,runtimeImage.loadBase,
                                 runtimeImage.loadBase+runtimeImage.entryRva);
    const bool unsupportedImportRejected=
        !unsupportedPeMachine.run() &&
        unsupportedPeMachine.cpu.error.find("neither guest code nor a registered API thunk")!=std::string::npos;

    bool pass=a.rfind("PASS",0)==0&&b.rfind("PASS",0)==0&&c.rfind("PASS",0)==0&&
              d.rfind("PASS",0)==0&&vm.rfind("PASS",0)==0&&peMapPass&&overlapRejected&&
              apiCallPass&&iatCallPass&&lastErrorCallPass&&exitCallPass&&importResolverPass&&
              peRuntimeCallPass&&unsupportedImportRejected;
    return std::string("ASTERPC CPU + WIN32 THUNKS + GUEST MEMORY + PE MAPPER: ")+(pass?"ALL TESTS PASS":"TEST FAILURE")+
      "\n1 MOV/ADD: "+a+"\n2 memory store/load: "+b+"\n3 register arithmetic: "+c+"\n4 shift: "+d+
      "\n5 guest virtual memory: "+vm+
      "\n6 PE section mapping: "+(peMapPass?"PASS: synthetic PE32+ image mapped with RX section permissions":"FAIL: "+peMap)+
      "\n7 PE overlap guard: "+(overlapRejected?"PASS: overlapping section rejected":"FAIL: "+overlapResult)+
      "\n8 CPU indirect CALL RAX -> GetTickCount64 thunk: "+(apiCallPass?"PASS":"FAIL")+
      "\n9 x64 IAT-style CALL [RIP+disp32] -> GetTickCount64: "+(iatCallPass?"PASS":"FAIL")+
      "\n10 CPU CALL -> SetLastError/GetLastError: "+(lastErrorCallPass?"PASS":"FAIL")+
      "\n11 CPU CALL -> ExitProcess guest stop: "+(exitCallPass?"PASS":"FAIL")+
      "\n12 PE import resolution: "+(importResolverPass?"PASS: supported API thunk separated from unresolved import":"FAIL: "+importMap)+
      "\n13 mapped PE IAT -> CPU CALL -> GetTickCount64: "+(peRuntimeCallPass?"PASS":"FAIL")+
      "\n14 unsupported PE IAT import remains non-callable: "+(unsupportedImportRejected?"PASS":"FAIL")+
      "\nGuest RAM: 1 MiB minimum, bounds-checked, little-endian; stack resides in guest RAM."+
      "\nPE runtime test: mapped PE image bytes and IAT are consumed directly by the experimental CPU; supported imports bind to deterministic API thunks."+
      "\nVirtual memory prototype: isolated regions, overlap rejection, read/write/execute permissions, 256 MiB commit cap."+
      "\nInstructions: MOV, LEA, ADD, SUB, OR, AND, XOR, CMP, TEST, INC, DEC, NOT, NEG, SHL, SHR, SAR, PUSH, POP, CALL, RET, JMP, JZ, JNZ, NOP, HLT."+
      "\nScope: experimental 64-bit subset; API calls use registered test thunks only. No complete PE-to-runtime loader, hardware paging/MMU, SIMD, interrupts, or broad Win32 compatibility.";
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_asterpc_MainActivity_runSelfTest(JNIEnv* env,jobject) {
    std::string out=selfTests(); return env->NewStringUTF(out.c_str());
}


// Guest display prototype: a bounded RGBA framebuffer exposed to the Android renderer.
// The current pixels are a diagnostic test pattern; CPU/Windows output is not connected yet.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_asterpc_GpuRenderer_nativeGetFramebuffer(JNIEnv* env, jclass, jint width, jint height) {
    if (width <= 0 || height <= 0 || width > 512 || height > 512) return nullptr;
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    std::vector<jbyte> pixels(bytes, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            const bool grid = ((x / 16) + (y / 16)) % 2 == 0;
            pixels[i + 0] = static_cast<jbyte>((x * 255) / width);
            pixels[i + 1] = static_cast<jbyte>((y * 255) / height);
            pixels[i + 2] = static_cast<jbyte>(grid ? 190 : 55);
            pixels[i + 3] = static_cast<jbyte>(255);
        }
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(bytes));
    if (result != nullptr) env->SetByteArrayRegion(result, 0, static_cast<jsize>(bytes), pixels.data());
    return result;
}


namespace {
static bool peRead16(const std::vector<uint8_t>& b, size_t o, uint16_t& v) {
    if (o > b.size() || b.size() - o < 2) return false;
    v = uint16_t(b[o]) | (uint16_t(b[o + 1]) << 8); return true;
}
static bool peRead32(const std::vector<uint8_t>& b, size_t o, uint32_t& v) {
    if (o > b.size() || b.size() - o < 4) return false;
    v = uint32_t(b[o]) | (uint32_t(b[o+1]) << 8) | (uint32_t(b[o+2]) << 16) | (uint32_t(b[o+3]) << 24); return true;
}
static bool peRead64(const std::vector<uint8_t>& b, size_t o, uint64_t& v) {
    uint32_t lo, hi; if (!peRead32(b,o,lo) || !peRead32(b,o+4,hi)) return false;
    v = uint64_t(lo) | (uint64_t(hi) << 32); return true;
}
static std::string inspectPe(const std::vector<uint8_t>& b) {
    if (b.size() < 64 || b[0] != 'M' || b[1] != 'Z') return "INVALID: DOS MZ signature not found.";
    uint32_t peOff;
    if (!peRead32(b,0x3c,peOff) || peOff > b.size() || b.size()-peOff < 24)
        return "INVALID: PE header offset is outside the file.";
    if (b[peOff] != 'P' || b[peOff+1] != 'E' || b[peOff+2] != 0 || b[peOff+3] != 0)
        return "INVALID: PE signature not found.";
    uint16_t machine, sections, optionalSize;
    if (!peRead16(b,peOff+4,machine) || !peRead16(b,peOff+6,sections) ||
        !peRead16(b,peOff+20,optionalSize)) return "INVALID: truncated COFF header.";
    const size_t opt = size_t(peOff) + 24;
    if (optionalSize < 0x70 || opt > b.size() || optionalSize > b.size()-opt)
        return "INVALID: truncated optional header.";
    uint16_t magic; uint32_t entryRva; uint64_t imageBase;
    if (!peRead16(b,opt,magic) || !peRead32(b,opt+16,entryRva) || !peRead64(b,opt+24,imageBase))
        return "INVALID: optional header fields are truncated.";
    if (machine != 0x8664 || magic != 0x20b)
        return "UNSUPPORTED: this prototype inspector accepts PE32+ x86-64 only.\nMachine: " +
          std::string(machine == 0x14c ? "x86" : "other") + "\nOptional header: " +
          (magic == 0x10b ? "PE32" : "not PE32+");
    if (sections == 0 || sections > 96) return "INVALID: unreasonable section count.";
    const size_t table = opt + optionalSize;
    if (table > b.size() || size_t(sections) > (b.size()-table)/40)
        return "INVALID: section table exceeds file bounds.";
    std::ostringstream out;
    out << "PE32+ x86-64 file detected\n"
        << "File size: " << b.size() << " bytes\n"
        << "Sections: " << sections << "\n"
        << "Image base: 0x" << std::hex << imageBase << "\n"
        << "Entry RVA: 0x" << entryRva << std::dec << "\n"
        << "Section table:\n";
    for (unsigned i=0; i<sections; ++i) {
        const size_t s=table+size_t(i)*40;
        char name[9]{}; std::memcpy(name,b.data()+s,8);
        uint32_t virtualSize, virtualAddress, rawSize, rawOffset;
        if (!peRead32(b,s+8,virtualSize) || !peRead32(b,s+12,virtualAddress) ||
            !peRead32(b,s+16,rawSize) || !peRead32(b,s+20,rawOffset))
            return "INVALID: malformed section header.";
        if (rawSize && (rawOffset > b.size() || rawSize > b.size()-rawOffset))
            return "INVALID: section raw data exceeds file bounds.";
        out << "  " << name << " RVA=0x" << std::hex << virtualAddress
            << " virtual=" << virtualSize << " raw=" << rawSize << std::dec << "\n";
    }
    out << "Result: structure parsed; image is NOT mapped or executed.\n"
        << "Not yet supported: relocations, imports, TLS, DLL loading, Win32 APIs.";
    return out.str();
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_asterpc_MainActivity_inspectPe(JNIEnv* env, jobject, jbyteArray input) {
    if (input == nullptr) return env->NewStringUTF("No file data received.");
    const jsize length = env->GetArrayLength(input);
    if (length <= 0 || length > 64 * 1024 * 1024)
        return env->NewStringUTF("File rejected: size must be between 1 byte and 64 MiB.");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    env->GetByteArrayRegion(input, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return env->NewStringUTF("Could not read file bytes."); }
    const std::string result = inspectPe(bytes);
    return env->NewStringUTF(result.c_str());
}


namespace {
static std::string mapPeImage(const std::vector<uint8_t>& b,PeRuntimeImage* runtime) {
    if(runtime) { runtime->bytes.clear(); runtime->imports.clear(); runtime->loadBase=0; runtime->entryRva=0; }
    if (b.size()<64 || b[0]!='M' || b[1]!='Z') return "INVALID: DOS MZ signature not found.";
    uint32_t peOff;
    if (!peRead32(b,0x3c,peOff) || size_t(peOff)>b.size() || b.size()-size_t(peOff)<24)
        return "INVALID: PE header offset is outside the file.";
    const size_t p=static_cast<size_t>(peOff);
    if (b[p]!='P'||b[p+1]!='E'||b[p+2]!=0||b[p+3]!=0) return "INVALID: PE signature not found.";
    uint16_t machine,sections,optionalSize;
    if(!peRead16(b,p+4,machine)||!peRead16(b,p+6,sections)||!peRead16(b,p+20,optionalSize))
        return "INVALID: truncated COFF header.";
    const size_t opt=p+24;
    if(machine!=0x8664) return "UNSUPPORTED: only x86-64 PE images are accepted.";
    if(opt>b.size()||optionalSize>b.size()-opt||optionalSize<112)
        return "INVALID: truncated PE32+ optional header.";
    uint16_t magic;
    uint32_t entry,sectionAlign,fileAlign,imageSize,headersSize,directoryCount;
    uint64_t preferredBase;
    if(!peRead16(b,opt,magic)||!peRead32(b,opt+16,entry)||!peRead64(b,opt+24,preferredBase)||
       !peRead32(b,opt+32,sectionAlign)||!peRead32(b,opt+36,fileAlign)||
       !peRead32(b,opt+56,imageSize)||!peRead32(b,opt+60,headersSize)||
       !peRead32(b,opt+108,directoryCount))
        return "INVALID: incomplete PE32+ optional header.";
    if(magic!=0x20b) return "UNSUPPORTED: PE32+ format required.";
    if(!sections||sections>96) return "INVALID: unreasonable section count.";
    if(!imageSize||imageSize>128u*1024u*1024u)
        return "REJECTED: SizeOfImage must be between 1 byte and 128 MiB.";
    if(!headersSize||headersSize>b.size()||headersSize>imageSize)
        return "INVALID: SizeOfHeaders is outside file/image bounds.";
    if(!sectionAlign||!fileAlign||(sectionAlign&(sectionAlign-1))||(fileAlign&(fileAlign-1))||
       fileAlign>65536||sectionAlign<fileAlign)
        return "INVALID: unsupported or inconsistent PE alignment.";
    if(entry>=imageSize) return "INVALID: entry point RVA exceeds image.";
    if(directoryCount>(optionalSize-112)/8)
        return "INVALID: data-directory count exceeds optional-header bounds.";
    const size_t table=opt+optionalSize;
    if(table>b.size()||size_t(sections)>(b.size()-table)/40)
        return "INVALID: section table exceeds file bounds.";
    std::vector<uint8_t> image(static_cast<size_t>(imageSize),0);
    std::memcpy(image.data(),b.data(),headersSize);
    for(unsigned i=0;i<sections;++i) {
        const size_t sh=table+size_t(i)*40;
        uint32_t vs,va,rs,ro;
        if(!peRead32(b,sh+8,vs)||!peRead32(b,sh+12,va)||!peRead32(b,sh+16,rs)||!peRead32(b,sh+20,ro))
            return "INVALID: malformed section header.";
        const uint64_t span=std::max<uint64_t>(vs,rs);
        if(span&&(va>imageSize||span>uint64_t(imageSize)-va))
            return "INVALID: section virtual range exceeds SizeOfImage.";
        if(rs&&(ro>b.size()||rs>b.size()-ro)) return "INVALID: section raw data exceeds file bounds.";
        if(rs) std::memcpy(image.data()+va,b.data()+ro,rs);
    }

    uint32_t relocRva=0,relocSize=0,importRva=0,importSize=0;
    auto readDirectory=[&](uint32_t index,uint32_t& rva,uint32_t& size)->bool {
        if(index>=directoryCount) return true;
        const size_t off=opt+112+size_t(index)*8;
        return peRead32(b,off,rva)&&peRead32(b,off+4,size);
    };
    if(!readDirectory(5,relocRva,relocSize)||!readDirectory(1,importRva,importSize))
        return "INVALID: truncated data-directory entry.";
    if((relocRva==0)!=(relocSize==0)||(importRva==0)!=(importSize==0))
        return "INVALID: data-directory RVA/size pair is inconsistent.";

    // This prototype uses a fixed guest load address and applies only AMD64 DIR64 relocations.
    constexpr uint64_t guestLoadBase=0x0000000010000000ULL;
    const uint64_t delta=guestLoadBase-preferredBase;
    uint32_t relocBlocks=0,relocEntries=0,dir64Entries=0;
    if(delta!=0 && relocRva==0)
        return "UNSUPPORTED: image requires relocation but has no base-relocation directory.";
    if(relocRva) {
        if(relocRva>image.size()||relocSize>image.size()-relocRva)
            return "INVALID: base relocation directory is outside mapped image.";
        size_t pos=relocRva,end=size_t(relocRva)+relocSize;
        while(pos<end) {
            if(end-pos<8) return "INVALID: truncated relocation block.";
            uint32_t pageRva,blockSize;
            if(!peRead32(image,pos,pageRva)||!peRead32(image,pos+4,blockSize)||
               blockSize<8||blockSize>end-pos||((blockSize-8)&1)||pageRva>=image.size())
                return "INVALID: malformed relocation block.";
            const uint32_t count=(blockSize-8)/2;
            for(uint32_t k=0;k<count;++k) {
                uint16_t item;
                if(!peRead16(image,pos+8+size_t(k)*2,item)) return "INVALID: truncated relocation entry.";
                const unsigned type=item>>12;
                const uint64_t target=uint64_t(pageRva)+(item&0x0fff);
                if(type==10) {
                    if(target>image.size()||image.size()-static_cast<size_t>(target)<8)
                        return "INVALID: DIR64 relocation target is outside image.";
                    uint64_t oldValue=0;
                    if(!peRead64(image,static_cast<size_t>(target),oldValue))
                        return "INVALID: cannot read DIR64 relocation target.";
                    const uint64_t relocated=oldValue+delta;
                    for(unsigned n=0;n<8;++n) image[static_cast<size_t>(target)+n]=uint8_t(relocated>>(n*8));
                    ++dir64Entries;
                } else if(type!=0) {
                    return "UNSUPPORTED: relocation type other than ABSOLUTE/DIR64 encountered.";
                }
                if(type!=0) ++relocEntries;
            }
            ++relocBlocks;
            pos+=blockSize;
        }
    }

    // Imports are parsed and IAT entries receive non-executable placeholder thunk addresses.
    // These addresses are diagnostic only; no DLL or Windows API is invoked.
    std::vector<std::string> importDetails;
    std::vector<PeImportBinding> runtimeImports;
    uint32_t importSymbols=0,importDllCount=0,recognizedApis=0,unresolvedImports=0;
    GuestModuleRegistry moduleRegistry;
    constexpr uint64_t importStubBase=API_STUB_BASE;
    constexpr uint64_t unresolvedStubBase=0x0000710000000000ULL;
    asterpc::WinApiDispatcher importDispatcher;
    auto isKnownApi=[&](const std::string& dll,const std::string& api)->bool {
        return importDispatcher.supports(dll,api);
    };
    if(importRva) {
        if(importRva>image.size()||importSize>image.size()-importRva||importSize<20)
            return "INVALID: import directory is outside mapped image.";
        size_t pos=importRva,end=size_t(importRva)+importSize;
        bool terminated=false;
        for(unsigned desc=0;desc<256&&pos+20<=end;++desc,pos+=20) {
            uint32_t oft=0,time=0,chain=0,nameRva=0,firstThunk=0;
            if(!peRead32(image,pos,oft)||!peRead32(image,pos+4,time)||!peRead32(image,pos+8,chain)||
               !peRead32(image,pos+12,nameRva)||!peRead32(image,pos+16,firstThunk))
                return "INVALID: truncated import descriptor.";
            if(!(oft|time|chain|nameRva|firstThunk)) { terminated=true; break; }
            if(!nameRva||nameRva>=image.size()||!firstThunk||firstThunk>=image.size())
                return "INVALID: import descriptor contains invalid RVA.";
            std::string dll;
            for(size_t q=nameRva;q<image.size()&&q-nameRva<260&&image[q];++q)
                dll.push_back(static_cast<char>(image[q]));
            if(dll.empty()||dll.size()>=260) return "INVALID: invalid import DLL name.";
            if(!moduleRegistry.registerImportModule(dll))
                return "REJECTED: import module registry limit exceeded.";
            ++importDllCount;
            if(importDetails.size()<64) importDetails.push_back("DLL "+dll);
            const uint32_t lookup=oft?oft:firstThunk;
            bool thunkTerminated=false;
            for(uint32_t ti=0;ti<4096;++ti) {
                const uint64_t src=uint64_t(lookup)+uint64_t(ti)*8;
                const uint64_t dst=uint64_t(firstThunk)+uint64_t(ti)*8;
                if(src>image.size()||image.size()-static_cast<size_t>(src)<8||
                   dst>image.size()||image.size()-static_cast<size_t>(dst)<8)
                    return "INVALID: import thunk/IAT table exceeds image.";
                uint64_t value=0;
                if(!peRead64(image,static_cast<size_t>(src),value))
                    return "INVALID: truncated import thunk.";
                if(!value) { thunkTerminated=true; break; }
                std::string symbol;
                if(value&(uint64_t(1)<<63)) {
                    if(value&0x7fffffffffff0000ULL)
                        return "INVALID: malformed ordinal import encoding.";
                    symbol="#"+std::to_string(value&0xffff);
                } else {
                    if(value>UINT32_MAX||value>=image.size()||image.size()-static_cast<size_t>(value)<3)
                        return "INVALID: import-by-name RVA is outside image.";
                    const size_t nameStart=static_cast<size_t>(value)+2;
                    for(size_t q=nameStart;q<image.size()&&q-nameStart<256&&image[q];++q)
                        symbol.push_back(static_cast<char>(image[q]));
                    if(symbol.empty()||symbol.size()>=256)
                        return "INVALID: malformed import symbol name.";
                }
                const bool known=isKnownApi(dll,symbol);
                if(known) ++recognizedApis;
                if(!moduleRegistry.noteImport(dll,known))
                    return "INVALID: import module registry state mismatch.";
                const uint64_t stub=known
                    ? importStubBase+uint64_t(recognizedApis-1)*16
                    : unresolvedStubBase+uint64_t(unresolvedImports)*16;
                for(unsigned n=0;n<8;++n) image[static_cast<size_t>(dst)+n]=uint8_t(stub>>(n*8));
                runtimeImports.push_back({dll,symbol,guestLoadBase+dst,stub,known});
                if(importDetails.size()<64)
                    importDetails.push_back("  "+symbol+" -> "+
                        (known?"callable Win32 API thunk":"unresolved non-callable placeholder"));
                if(!known) ++unresolvedImports;
                ++importSymbols;
            }
            if(!thunkTerminated) return "INVALID: import thunk limit exceeded.";
        }
        if(!terminated) return "INVALID: import descriptor table has no terminator.";
    }

    // Commit PE headers and sections into a bounded guest address space.
    GuestAddressSpace guestSpace;
    if(!guestSpace.mapBytes(guestLoadBase,image,0,headersSize,true,false,false))
        return "REJECTED: PE headers could not be mapped into guest address space.";
    uint32_t executableSections=0,writableSections=0;
    for(unsigned i=0;i<sections;++i) {
        const size_t sh=table+size_t(i)*40;
        uint32_t vs=0,va=0,rs=0,ro=0,characteristics=0;
        if(!peRead32(b,sh+8,vs)||!peRead32(b,sh+12,va)||!peRead32(b,sh+16,rs)||
           !peRead32(b,sh+20,ro)||!peRead32(b,sh+36,characteristics))
            return "INVALID: section permission metadata is truncated.";
        (void)ro;
        const size_t span=static_cast<size_t>(std::max<uint64_t>(vs,rs));
        if(!span) continue;
        const bool executable=(characteristics&0x20000000u)!=0;
        const bool writable=(characteristics&0x80000000u)!=0;
        const bool readable=(characteristics&0x40000000u)!=0 || executable;
        if(!guestSpace.mapBytes(guestLoadBase+va,image,va,span,readable,writable,executable))
            return "REJECTED: overlapping PE sections or guest memory limit exceeded.";
        if(executable) ++executableSections;
        if(writable) ++writableSections;
    }
    if(!guestSpace.canFetch(guestLoadBase+entry))
        return "REJECTED: PE entry point is not inside a readable executable section.";

    std::ostringstream out;
    out<<"PE32+ IMAGE PREPARED IN TEMPORARY GUEST BUFFER\n"
       <<"Architecture: x86-64\nPreferred image base: 0x"<<std::hex<<preferredBase
       <<"\nGuest load base: 0x"<<guestLoadBase<<"\nEntry RVA: 0x"<<entry<<std::dec
       <<"\nMapped image size: "<<image.size()<<" bytes\nSections mapped: "<<sections
       <<"\nBase relocations applied: "<<relocEntries<<" (DIR64: "<<dir64Entries
       <<", blocks: "<<relocBlocks<<")\nImport DLLs: "<<importDllCount
       <<"\nIAT entries assigned addresses: "<<importSymbols
       <<"\nCallable Win32 API thunks (subset): "<<recognizedApis
       <<"\nUnresolved imports: "<<unresolvedImports
       <<"\nGuest memory regions: "<<guestSpace.regionCount()
       <<" ("<<guestSpace.committedBytes()<<" bytes committed)"
       <<"\nExecutable sections: "<<executableSections
       <<"\nWritable sections: "<<writableSections
       <<"\nDLL metadata records: "<<moduleRegistry.size()<<"\n";
    out<<moduleRegistry.describe();
    for(const auto& detail:importDetails) out<<"  "<<detail<<"\n";
    out<<"BSS/uninitialized bytes: zero-filled\n"
       <<"Headers and sections were copied into bounded guest address-space regions.\n"
       <<"Supported IAT entries can be bound to the deterministic CPU API dispatcher; unsupported entries remain non-callable.\n"
       <<"This mapper prepares an image for controlled tests; user-selected PE files are not launched.\n"
       <<"Not implemented: real DLL loading, TLS, process startup, Windows subsystem services, and full Win32 compatibility.";
    if(runtime) {
        runtime->bytes=std::move(image);
        runtime->loadBase=guestLoadBase;
        runtime->entryRva=entry;
        runtime->imports=std::move(runtimeImports);
    }
    return out.str();
}

}

extern "C" JNIEXPORT jstring JNICALL
Java_com_asterpc_MainActivity_mapPeImage(JNIEnv* env,jobject,jbyteArray input) {
    if (input==nullptr) return env->NewStringUTF("No file data received.");
    const jsize length=env->GetArrayLength(input);
    if (length<=0 || length>64*1024*1024)
        return env->NewStringUTF("File rejected: size must be between 1 byte and 64 MiB.");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    env->GetByteArrayRegion(input,0,length,reinterpret_cast<jbyte*>(bytes.data()));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return env->NewStringUTF("Could not read file bytes."); }
    const std::string result=mapPeImage(bytes);
    return env->NewStringUTF(result.c_str());
}
