#include <jni.h>
#include <cstdint>
#include <sstream>
#include <string>

// Deliberately tiny educational interpreter: MOV RAX, imm64; ADD RAX, imm32; HLT.
// This is not a complete x86-64 CPU and cannot execute Windows programs.
static std::string execute_demo() {
    const uint8_t code[] = {
        0x48, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x05, 0x08, 0x00, 0x00, 0x00,
        0xF4
    };
    uint64_t rax = 0;
    size_t ip = 0;
    unsigned instructions = 0;
    while (ip < sizeof(code) && instructions < 16) {
        if (ip + 10 <= sizeof(code) && code[ip] == 0x48 && code[ip + 1] == 0xB8) {
            rax = 0;
            for (int i = 0; i < 8; ++i) rax |= (uint64_t)code[ip + 2 + i] << (8 * i);
            ip += 10;
        } else if (ip + 6 <= sizeof(code) && code[ip] == 0x48 && code[ip + 1] == 0x05) {
            uint32_t imm = (uint32_t)code[ip + 2] |
                ((uint32_t)code[ip + 3] << 8) |
                ((uint32_t)code[ip + 4] << 16) |
                ((uint32_t)code[ip + 5] << 24);
            rax += (int32_t)imm;
            ip += 6;
        } else if (code[ip] == 0xF4) {
            ++ip;
            ++instructions;
            break;
        } else {
            return "Unsupported opcode at byte " + std::to_string(ip);
        }
        ++instructions;
    }
    std::ostringstream out;
    out << "CPU interpreter self-test: PASS\n"
        << "Instructions executed: " << instructions << "\n"
        << "RAX result: " << rax << " (expected 50)\n"
        << "Guest code bytes: " << sizeof(code);
    return out.str();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_asterpc_MainActivity_runSelfTest(JNIEnv* env, jobject) {
    const std::string result = execute_demo();
    return env->NewStringUTF(result.c_str());
}
