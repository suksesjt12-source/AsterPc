# AsterPC 0.7.0

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

## Limited Win32 API dispatcher
- Separate deterministic dispatcher prototype using the Microsoft x64 integer argument registers (RCX, RDX, R8, R9).
- Implemented test semantics for `GetTickCount`, `GetTickCount64`, `GetLastError`, `SetLastError`, `GetCurrentProcess`, and `ExitProcess` from `kernel32.dll`/`kernelbase.dll`.
- Supports a virtual tick source and isolated guest last-error / exit-state fields; it does not call host Windows APIs or affect Android.
- Includes a separate native self-test exposed by the Android UI.

**Integration status:** the x86-64 interpreter can now execute indirect `CALL r/m64` instructions targeting explicitly registered API thunk addresses. The bridge transfers `RCX`, `RDX`, `R8`, and `R9`, returns API results through `RAX`, and turns `ExitProcess` into a guest stop request. Self-tests cover `GetTickCount64`, `SetLastError`/`GetLastError`, and `ExitProcess`. The PE parser still writes diagnostic placeholder addresses rather than automatically registering each imported API; this is not yet a complete PE runtime or Windows process.

## GPU
Aplikasi menampilkan framebuffer RGBA sintetis melalui JNI dan OpenGL ES 2.0. Ini hanya pola uji, bukan output Windows, bukan penerjemah DirectX/Vulkan, dan belum terhubung ke CPU guest.

## Pengujian aplikasi
Tombol **Run CPU + Windows API integration self-test** menjalankan:
- Tes MOV/ADD, memory read/write, arithmetic, dan shift.
- Tes pemetaan region, akses permission, dan penolakan overlap.
- Tes registry DLL metadata dan deduplikasi nama yang tidak peka huruf besar/kecil.
- Tes smoke PE32+ sintetis dan penolakan section overlap.
- Tes CPU indirect `CALL` ke thunk `GetTickCount64`.
- Tes alur `SetLastError` lalu `GetLastError` melalui register CPU.
- Tes `ExitProcess` menghentikan eksekusi guest dengan exit code yang diminta.

Tes ini adalah tes internal sederhana, bukan verifikasi kompatibilitas Windows atau validasi menyeluruh terhadap file PE dari dunia nyata.

## Build
Jalankan workflow **Android APK** melalui GitHub Actions. Artifact bernama `AsterPC-debug-apk`.

Target ABI: `arm64-v8a`.
