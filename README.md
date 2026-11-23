# AsterPC

AsterPC adalah prototipe riset emulator PC untuk Android ARM64.

## Status versi 0.1.0
- Kerangka aplikasi Android native.
- JNI + C++17.
- Interpreter demonstrasi untuk subset instruksi x86-64: MOV RAX, imm64; ADD RAX, imm32; HLT.
- GitHub Actions untuk membangun APK debug.

## Batasan penting
Ini **belum** emulator Windows dan belum dapat menjalankan file EXE atau game PC. Belum tersedia loader PE, Windows API/NT layer, virtual memory guest lengkap, DirectX/Vulkan translation, audio, input mapping, atau kompatibilitas game. Self-test hanya menjalankan potongan kode instruksi buatan yang sangat kecil.

## Build
Jalankan workflow **Android APK** melalui GitHub Actions. Artifact bernama `AsterPC-debug-apk`.

Target ABI: `arm64-v8a`.
