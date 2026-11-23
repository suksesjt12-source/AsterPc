#include <jni.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
enum Reg : unsigned { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
                      R8, R9, R10, R11, R12, R13, R14, R15 };

struct Cpu {
    std::array<uint64_t, 16> r{};
    uint64_t rip = 0;
    bool zf = false, sf = false, cf = false, of = false;
    bool halted = false;
    uint64_t steps = 0;
    std::string error;
};

struct Machine {
    Cpu cpu;
    const std::vector<uint8_t>& code;
    explicit Machine(const std::vector<uint8_t>& bytes) : code(bytes) {
        cpu.r[RSP] = 0x100000;
    }
    bool fetch8(uint8_t& v) {
        if (cpu.rip >= code.size()) return fail("instruction pointer out of range");
        v = code[static_cast<size_t>(cpu.rip++)]; return true;
    }
    bool fetch32(uint32_t& v) {
        if (cpu.rip + 4 > code.size()) return fail("truncated 32-bit immediate");
        v = 0; for (int i=0;i<4;i++) v |= uint32_t(code[cpu.rip++]) << (i*8);
        return true;
    }
    bool fetch64(uint64_t& v) {
        if (cpu.rip + 8 > code.size()) return fail("truncated 64-bit immediate");
        v = 0; for (int i=0;i<8;i++) v |= uint64_t(code[cpu.rip++]) << (i*8);
        return true;
    }
    bool fail(const char* s) { cpu.error=s; return false; }
    void logicFlags(uint64_t v) { cpu.zf=(v==0); cpu.sf=(v>>63)!=0; cpu.cf=false; cpu.of=false; }
    void add(uint64_t& a, uint64_t b) {
        uint64_t old=a; a+=b; cpu.zf=(a==0); cpu.sf=(a>>63)!=0;
        cpu.cf=(a<old); cpu.of=((~(old^b)&(old^a))>>63)!=0;
    }
    void sub(uint64_t& a, uint64_t b) {
        uint64_t old=a; a-=b; cpu.zf=(a==0); cpu.sf=(a>>63)!=0;
        cpu.cf=(old<b); cpu.of=(((old^b)&(old^a))>>63)!=0;
    }
    bool push(uint64_t v) {
        if (cpu.r[RSP] < 8) return fail("stack underflow");
        cpu.r[RSP]-=8; stack[cpu.r[RSP]]=v; return true;
    }
    bool pop(uint64_t& v) {
        auto it=stack.find(cpu.r[RSP]);
        if (it==stack.end()) return fail("invalid stack read");
        v=it->second; stack.erase(it); cpu.r[RSP]+=8; return true;
    }
    bool run(uint64_t limit=100000) {
        while (!cpu.halted && cpu.steps < limit) {
            uint8_t op; if(!fetch8(op)) return false;
            cpu.steps++;
            if(op==0x90) continue;                         // NOP
            if(op==0xF4) { cpu.halted=true; break; }       // HLT
            if(op>=0xB8 && op<=0xBF) {                     // MOV r64, imm64
                uint64_t imm; if(!fetch64(imm)) return false;
                cpu.r[op-0xB8]=imm; continue;
            }
            if(op>=0x50 && op<=0x57) { if(!push(cpu.r[op-0x50])) return false; continue; }
            if(op>=0x58 && op<=0x5F) { uint64_t v; if(!pop(v)) return false; cpu.r[op-0x58]=v; continue; }
            if(op==0xEB || op==0x74 || op==0x75) {
                uint8_t raw; if(!fetch8(raw)) return false;
                int8_t rel=static_cast<int8_t>(raw);
                bool take=(op==0xEB)||(op==0x74&&cpu.zf)||(op==0x75&&!cpu.zf);
                if(take) { int64_t dest=int64_t(cpu.rip)+rel; if(dest<0||uint64_t(dest)>=code.size()) return fail("branch target out of range"); cpu.rip=uint64_t(dest); }
                continue;
            }
            if(op==0xE8 || op==0xE9) {
                uint32_t raw; if(!fetch32(raw)) return false;
                int32_t rel; std::memcpy(&rel,&raw,4);
                int64_t dest=int64_t(cpu.rip)+rel;
                if(dest<0||uint64_t(dest)>=code.size()) return fail("relative target out of range");
                if(op==0xE8 && !push(cpu.rip)) return false;
                cpu.rip=uint64_t(dest); continue;
            }
            if(op==0xC3) { uint64_t dest; if(!pop(dest)) return false; if(dest>=code.size()) return fail("return target out of range"); cpu.rip=dest; continue; }

            // Supported REX.W register-register instructions; memory addressing is intentionally rejected.
            uint8_t rex=0;
            if(op>=0x40 && op<=0x4F) { rex=op; if(!fetch8(op)) return false; }
            bool wide=(rex&8)!=0;
            if(!wide) return fail("only 64-bit REX.W operations are supported");
            if(op==0x89 || op==0x8B || op==0x01 || op==0x03 ||
               op==0x29 || op==0x2B || op==0x31 || op==0x33 ||
               op==0x39 || op==0x3B) {
                uint8_t m; if(!fetch8(m)) return false;
                if((m>>6)!=3) return fail("memory operands are not implemented");
                unsigned reg=((m>>3)&7)+((rex&4)?8:0);
                unsigned rm=(m&7)+((rex&1)?8:0);
                if(op==0x89) cpu.r[rm]=cpu.r[reg];
                else if(op==0x8B) cpu.r[reg]=cpu.r[rm];
                else if(op==0x01) add(cpu.r[rm],cpu.r[reg]);
                else if(op==0x03) add(cpu.r[reg],cpu.r[rm]);
                else if(op==0x29) sub(cpu.r[rm],cpu.r[reg]);
                else if(op==0x2B) sub(cpu.r[reg],cpu.r[rm]);
                else if(op==0x31) { cpu.r[rm]^=cpu.r[reg]; logicFlags(cpu.r[rm]); }
                else if(op==0x33) { cpu.r[reg]^=cpu.r[rm]; logicFlags(cpu.r[reg]); }
                else if(op==0x39) { uint64_t t=cpu.r[rm]; sub(t,cpu.r[reg]); }
                else { uint64_t t=cpu.r[reg]; sub(t,cpu.r[rm]); }
                continue;
            }
            if(op==0x81 || op==0x83) {
                uint8_t m; if(!fetch8(m)) return false;
                if((m>>6)!=3) return fail("immediate memory operands are not implemented");
                unsigned target=(m&7)+((rex&1)?8:0), group=(m>>3)&7;
                uint64_t imm=0;
                if(op==0x81) { uint32_t x; if(!fetch32(x)) return false; imm=static_cast<int32_t>(x); }
                else { uint8_t x; if(!fetch8(x)) return false; imm=static_cast<int8_t>(x); }
                if(group==0) add(cpu.r[target],imm);
                else if(group==5) sub(cpu.r[target],imm);
                else if(group==6) { cpu.r[target]^=imm; logicFlags(cpu.r[target]); }
                else if(group==7) { uint64_t t=cpu.r[target]; sub(t,imm); }
                else return fail("unsupported immediate arithmetic group");
                continue;
            }
            return fail("unsupported opcode");
        }
        if(!cpu.halted && cpu.steps>=limit) return fail("instruction limit reached");
        return true;
    }
    std::unordered_map<uint64_t,uint64_t> stack;
};
}

static std::string runProgram(const std::vector<uint8_t>& bytes, uint64_t expected) {
    Machine m(bytes);
    bool ok=m.run();
    std::ostringstream s;
    s << (ok && m.cpu.r[RAX]==expected ? "PASS" : "FAIL")
      << " | RAX=" << m.cpu.r[RAX] << " | steps=" << m.cpu.steps;
    if(!ok) s << " | error=" << m.cpu.error;
    else if(m.cpu.r[RAX]!=expected) s << " | expected=" << expected;
    return s.str();
}

static std::string execute_self_tests() {
    // MOV RAX,42; ADD RAX,8; HLT
    const std::vector<uint8_t> basic={0x48,0xB8,42,0,0,0,0,0,0,0,0x48,0x83,0xC0,8,0xF4};
    // MOV RAX,9; MOV RCX,3; ADD RAX,RCX; SUB RAX,1; HLT
    const std::vector<uint8_t> arithmetic={0x48,0xB8,9,0,0,0,0,0,0,0,
        0x48,0xB9,3,0,0,0,0,0,0,0,0x48,0x01,0xC8,0x48,0x83,0xE8,1,0xF4};
    // MOV RAX,7; PUSH RAX; MOV RAX,0; POP RCX; MOV RAX,RCX; HLT
    const std::vector<uint8_t> stackTest={0x48,0xB8,7,0,0,0,0,0,0,0,0x50,
        0x48,0xB8,0,0,0,0,0,0,0,0,0x59,0x48,0x89,0xC8,0xF4};
    std::string a=runProgram(basic,50), b=runProgram(arithmetic,11), c=runProgram(stackTest,7);
    bool pass=a.rfind("PASS",0)==0 && b.rfind("PASS",0)==0 && c.rfind("PASS",0)==0;
    return std::string("ASTERPC x86-64 interpreter: ")+(pass?"ALL TESTS PASS":"TEST FAILURE")+
        "\n1. MOV/ADD immediate: "+a+"\n2. Register arithmetic: "+b+
        "\n3. PUSH/POP stack: "+c+
        "\nSupported: MOV imm64, MOV reg, ADD, SUB, XOR, CMP, immediate arithmetic, PUSH/POP, JMP/JZ/JNZ, CALL/RET, NOP, HLT"+
        "\nLimitations: register-only operands; no SIMD, privilege mode, paging, interrupts, or Windows ABI.";
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_asterpc_MainActivity_runSelfTest(JNIEnv* env, jobject) {
    const std::string result=execute_self_tests();
    return env->NewStringUTF(result.c_str());
}
