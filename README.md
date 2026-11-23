# AsterPC

AsterPC adalah prototipe riset emulator PC untuk Android ARM64.

## Status CPU engine
- Aplikasi Android native dengan JNI + C++17.
- Interpreter x86-64 eksperimental dengan 16 register general-purpose 64-bit.
- Guest RAM 1 MiB dengan pembacaan/penulisan little-endian dan pemeriksaan batas alamat.
- Stack CPU menggunakan guest RAM.
- Decode ModRM dasar, displacement, dan SIB untuk operand register/memori.
- Flags dasar: ZF, SF, CF, OF.
- Batas eksekusi instruksi untuk mencegah loop tak terbatas.

## Instruksi yang didukung (subset)
MOV, LEA, ADD, SUB, OR, AND, XOR, CMP, TEST, INC, DEC, NOT, NEG, SHL, SHR, SAR, PUSH, POP, CALL, RET, JMP, JZ, JNZ, NOP, HLT.

Dukungan saat ini masih eksperimental dan terutama berfokus pada operasi integer 64-bit. Tidak semua encoding, prefix, ukuran operand, atau addressing mode x86-64 tersedia.

## Pengujian
Tombol **Run CPU interpreter self-test** menjalankan tes instruksi immediate, operasi register, akses guest memory, dan shift. Hasil tes ditampilkan di aplikasi.

## Batasan penting
Ini **belum** emulator Windows dan belum dapat menjalankan file EXE atau game PC. Belum tersedia loader PE, Windows API/NT layer, paging/virtual memory x86, SIMD/SSE/AVX, interrupt, privilege rings, DirectX/Vulkan translation, audio, input mapping, atau kompatibilitas game. Guest RAM 1 MiB ini adalah memori linear sederhana, bukan implementasi virtual memory Windows.

## Build
Jalankan workflow **Android APK** melalui GitHub Actions. Artifact bernama `AsterPC-debug-apk`.

Target ABI: `arm64-v8a`.
