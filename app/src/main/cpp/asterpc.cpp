#include <jni.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace {
enum Reg : unsigned { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
                      R8, R9, R10, R11, R12, R13, R14, R15 };
constexpr size_t MEMORY_SIZE = 1024 * 1024;
constexpr uint64_t DEFAULT_STEP_LIMIT = 1000000;

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
    explicit Machine(const std::vector<uint8_t>& program) : mem(MEMORY_SIZE,0) {
        if(program.size()>mem.size()/2) { cpu.error="program is larger than guest memory"; return; }
        std::copy(program.begin(),program.end(),mem.begin());
        cpu.r[RSP]=MEMORY_SIZE-16;
    }
    bool fail(const char* why) { cpu.error=why; return false; }
    bool range(uint64_t a,size_t n) const { return a<=mem.size() && n<=mem.size()-static_cast<size_t>(a); }
    bool fetch8(uint8_t& v) {
        if(!range(cpu.rip,1)) return fail("instruction fetch outside guest memory");
        v=mem[static_cast<size_t>(cpu.rip++)]; return true;
    }
    bool fetchN(uint64_t& v,unsigned n) {
        if(!range(cpu.rip,n)) return fail("truncated instruction immediate");
        v=0; for(unsigned i=0;i<n;i++) v|=uint64_t(mem[static_cast<size_t>(cpu.rip++)])<<(i*8);
        return true;
    }
    bool readMem(uint64_t a,unsigned n,uint64_t& v) {
        if(!range(a,n)) return fail("guest memory read out of bounds");
        v=0; for(unsigned i=0;i<n;i++) v|=uint64_t(mem[static_cast<size_t>(a)+i])<<(i*8);
        return true;
    }
    bool writeMem(uint64_t a,unsigned n,uint64_t v) {
        if(!range(a,n)) return fail("guest memory write out of bounds");
        for(unsigned i=0;i<n;i++) mem[static_cast<size_t>(a)+i]=uint8_t(v>>(i*8));
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
        uint64_t base=0; bool hasBase=true;
        if(rawRm==4) {
            uint8_t sib; if(!fetch8(sib)) return false;
            unsigned scale=sib>>6, idx=((sib>>3)&7)+((rex&2)?8:0), b=(sib&7)+((rex&1)?8:0);
            if(((sib>>3)&7)!=4 || (rex&2)) base+=cpu.r[idx] << scale;
            if(mod==0 && (sib&7)==5) hasBase=false; else base+=cpu.r[b];
        } else if(mod==0 && rawRm==5) hasBase=false;
        else base=cpu.r[rawRm+((rex&1)?8:0)];
        int64_t disp=0;
        if(mod==0 && !hasBase) { uint64_t d; if(!fetchN(d,4)) return false; int32_t x; std::memcpy(&x,&d,4); disp=x; }
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
        if(dest<0 || static_cast<uint64_t>(dest)>=mem.size()) return fail("branch target outside guest memory");
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
            if(op==0xC3) { uint64_t dest; if(!pop(dest)) return false; if(dest>=mem.size()) return fail("RET target outside guest memory"); cpu.rip=dest; continue; }

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
static std::string selfTests() {
    const std::vector<uint8_t> basic={0x48,0xB8,42,0,0,0,0,0,0,0,0x48,0x83,0xC0,8,0xF4};
    const std::vector<uint8_t> memtest={0x48,0xB8,0x00,0x01,0x00,0x00,0,0,0,0,0x48,0xC7,0x00,0x2A,0,0,0,0x48,0x8B,0x08,0x48,0x89,0xC8,0xF4};
    const std::vector<uint8_t> arithmetic={0x48,0xB8,9,0,0,0,0,0,0,0,0x48,0xB9,3,0,0,0,0,0,0,0,0x48,0x01,0xC8,0x48,0x83,0xE8,1,0xF4};
    const std::vector<uint8_t> shifts={0x48,0xB8,3,0,0,0,0,0,0,0,0x48,0xC1,0xE0,2,0xF4};
    std::string a=test(basic,50),b=test(memtest,42),c=test(arithmetic,11),d=test(shifts,12);
    bool pass=a.rfind("PASS",0)==0&&b.rfind("PASS",0)==0&&c.rfind("PASS",0)==0&&d.rfind("PASS",0)==0;
    return std::string("ASTERPC CPU + GUEST MEMORY: ")+(pass?"ALL TESTS PASS":"TEST FAILURE")+
      "\n1 MOV/ADD: "+a+"\n2 memory store/load: "+b+"\n3 register arithmetic: "+c+"\n4 shift: "+d+
      "\nGuest RAM: 1 MiB, bounds-checked, little-endian; stack resides in guest RAM."+
      "\nInstructions: MOV, LEA, ADD, SUB, OR, AND, XOR, CMP, TEST, INC, DEC, NOT, NEG, SHL, SHR, SAR, PUSH, POP, CALL, RET, JMP, JZ, JNZ, NOP, HLT."+
      "\nScope: experimental 64-bit subset; no paging/virtual memory, SIMD, interrupts, privilege rings, or Windows API.";
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_asterpc_MainActivity_runSelfTest(JNIEnv* env,jobject) {
    std::string out=selfTests(); return env->NewStringUTF(out.c_str());
}
