# AsterPC

AsterPC adalah prototipe riset emulator PC untuk Android ARM64. Proyek ini **belum dapat menjalankan aplikasi atau game Windows**.

## Status CPU engine
- Aplikasi Android native dengan JNI + C++17.
- Interpreter x86-64 eksperimental dengan 16 register general-purpose 64-bit.
- Guest RAM linear 1 MiB dengan pemeriksaan batas dan operasi little-endian.
- Stack CPU menggunakan guest RAM.
- Decode ModRM dasar, displacement, dan SIB untuk operand register/memori.
- Flags dasar: ZF, SF, CF, OF.
- Batas eksekusi instruksi untuk mencegah loop tak terbatas.
- Instruksi yang tersedia masih berupa subset eksperimental: MOV, LEA, ADD, SUB, OR, AND, XOR, CMP, TEST, INC, DEC, NOT, NEG, SHL, SHR, SAR, PUSH, POP, CALL, RET, JMP, JZ, JNZ, NOP, HLT.

Tidak semua encoding, prefix, ukuran operand, atau addressing mode x86-64 tersedia.

## Guest address space
- Region guest dapat dipetakan pada alamat dasar tertentu.
- Penolakan region yang saling tumpang tindih.
- Pemeriksaan akses baca, tulis, dan eksekusi.
- Batas alokasi region uji 256 MiB.
- Ini adalah model region memori in-process, **bukan** implementasi paging/MMU Windows atau pemetaan virtual memory hardware.

## PE32+ x86-64
- Pemeriksaan DOS MZ, PE signature, COFF, dan PE32+.
- Pemetaan header dan section ke model guest address space.
- Pengisian nol pada area BSS/uninitialized.
- Dukungan relokasi dasar AMD64 DIR64.
- Parsing import descriptor, import-by-name, serta import ordinal.
- Pemeriksaan permission section dari flag PE read/write/execute.
- Registry metadata untuk nama DLL import dan jumlah simbolnya.
- Daftar nama API umum untuk pengenalan saja.

**Batasan loader:** alamat IAT dan handle DLL masih placeholder diagnostik. DLL tidak dimuat atau dieksekusi. Windows API belum mempunyai dispatcher yang dapat dipanggil guest CPU. TLS, startup process, page-protection enforcement saat eksekusi, dan loader lengkap belum ada. PE image dipetakan hanya untuk pemeriksaan dan kemudian dibuang.

## GPU
Aplikasi menampilkan framebuffer RGBA sintetis melalui JNI dan OpenGL ES 2.0. Ini hanya pola uji, bukan output Windows, bukan penerjemah DirectX/Vulkan, dan belum terhubung ke CPU guest.

## Pengujian aplikasi
Tombol **Run CPU interpreter self-test** menjalankan:
- Tes MOV/ADD, memory read/write, arithmetic, dan shift.
- Tes pemetaan region, akses permission, dan penolakan overlap.
- Tes registry DLL metadata dan deduplikasi nama yang tidak peka huruf besar/kecil.
- Tes smoke PE32+ sintetis dan penolakan section overlap.

Tes ini adalah tes internal sederhana, bukan verifikasi kompatibilitas Windows atau validasi menyeluruh terhadap file PE dari dunia nyata.

## Build
Jalankan workflow **Android APK** melalui GitHub Actions. Artifact bernama `AsterPC-debug-apk`.

Target ABI: `arm64-v8a`.
