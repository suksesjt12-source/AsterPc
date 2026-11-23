# AsterPC 0.8.3

AsterPC adalah prototipe riset emulator PC untuk Android ARM64. Proyek ini **belum dapat menjalankan aplikasi atau game Windows**.

## Status CPU engine
- Aplikasi Android native dengan JNI + C++17.
- Interpreter x86-64 eksperimental dengan 16 register general-purpose 64-bit.
- Guest RAM linear minimal 1 MiB dengan pemeriksaan batas dan operasi little-endian; model CPU juga mendukung alamat dasar guest untuk menjalankan byte image pada alamat PE.
- Stack CPU menggunakan guest RAM.
- Decode ModRM dasar, displacement, SIB, dan RIP-relative untuk operand register/memori.
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
- Import API yang didukung dispatcher dibedakan dari import yang belum didukung; IAT image uji diberi alamat thunk untuk enam fungsi yang tersedia.

**Batasan loader:** self-test kini mengoper byte image PE sintetis dan IAT hasil pemetaan langsung ke CPU, lalu mengikat impor yang didukung ke API thunk. Bootstrap proses guest menyiapkan alamat image/entry point, stack guest terpisah, command line dan environment block UTF-16LE, serta registry image executable. DLL impor hanya dicatat sebagai metadata dan tidak benar-benar dimuat. Belum ada PEB/TEB Windows yang kompatibel atau struktur process parameters Windows; data bootstrap bukan pengganti struktur tersebut. Ini adalah process context eksperimental, bukan implementasi PEB/TEB, Windows process parameters, atau loader Windows yang kompatibel. File EXE pilihan pengguna hanya diperiksa/dipetakan; aplikasi tidak mencoba menjalankannya. TLS, process startup Windows yang nyata, enforcement permission saat instruksi berjalan, dan loader lengkap belum ada.

## Limited Win32 API dispatcher
- Dispatcher deterministik menggunakan register argumen integer Microsoft x64 (RCX, RDX, R8, R9).
- Semantik uji tersedia untuk `GetTickCount`, `GetTickCount64`, `GetLastError`, `SetLastError`, `GetCurrentProcess`, dan `ExitProcess` dari `kernel32.dll`/`kernelbase.dll`.
- Menggunakan waktu virtual dan status last-error/exit guest; tidak memanggil Windows API host atau memengaruhi Android.
- Menyediakan self-test native terpisah melalui UI Android.

**Integration status:** a synthetic process bootstrap builds a bounded guest stack, writes UTF-16LE command-line/environment blocks, and stores AsterPC-specific process/thread records in guest memory. The records contain deterministic PID/TID values, image/entry addresses, parameter pointers, stack bounds, and a process-to-thread record pointer. Their custom magic/version/layout is experimental and explicitly NOT compatible with Windows PEB/TEB. Imported DLLs remain metadata-only (not loaded). The CPU can execute indirect `CALL r/m64` from an image loaded at a guest base address. For the synthetic PE fixture, the mapper writes supported API thunk addresses into the actual IAT, returns import-binding metadata, and the test binds those addresses to the dispatcher. The CPU then fetches the mapped entry bytes and calls `GetTickCount64` through `CALL [RIP+disp32]`; a second test verifies that an unsupported IAT import fails as non-callable. The dispatcher uses `RCX`, `RDX`, `R8`, and `R9`, returns through `RAX`, and models `ExitProcess` as guest termination. This remains a controlled synthetic runtime smoke test, not execution of arbitrary Windows PE files.

## GPU
Aplikasi menampilkan framebuffer RGBA sintetis melalui JNI dan OpenGL ES 2.0. Ini hanya pola uji, bukan output Windows, bukan penerjemah DirectX/Vulkan, dan belum terhubung ke CPU guest.

## Pengujian aplikasi
Tombol **Run CPU + Windows API integration self-test** menjalankan:
- Tes MOV/ADD, memory read/write, arithmetic, dan shift.
- Tes pemetaan region, akses permission, dan penolakan overlap.
- Tes registry DLL metadata dan deduplikasi nama yang tidak peka huruf besar/kecil.
- Tes smoke PE32+ sintetis dan penolakan section overlap.
- Tes CPU indirect `CALL RAX` ke thunk `GetTickCount64`.
- Tes RIP-relative `CALL [RIP+disp32]` terhadap slot IAT sintetis.
- Tes resolver import PE dengan satu thunk API didukung dan satu impor non-callable.
- Tes runtime PE sintetis: byte image dan IAT hasil mapper dijalankan oleh CPU, lalu `CALL [RIP+disp32]` mencapai `GetTickCount64`.
- Tes bootstrap proses guest: entry point, guest stack 16-byte aligned, command line UTF-16LE, environment block UTF-16LE, dan registry modul.
- Validasi compile-time/runtime untuk pembacaan parameter proses pada self-test; hasil build sebelumnya gagal karena helper pembaca menerima `const Machine` meski `readMem` mencatat error pada state CPU.
- Tes bahwa executable dicatat sebagai mapped image sementara DLL impor tetap metadata-only dan tidak berstatus loaded.
- Tes bahwa pemanggilan IAT untuk impor yang belum didukung ditolak sebagai non-callable.
- Tes capability lookup dispatcher: nama DLL dinormalisasi, nama export tetap exact-match, dan API/DLL yang belum didukung ditolak.
- Tes alur `SetLastError` lalu `GetLastError` melalui register CPU.
- Tes `ExitProcess` menghentikan eksekusi guest dengan exit code yang diminta.

Tes ini adalah tes internal sederhana, bukan verifikasi kompatibilitas Windows atau validasi menyeluruh terhadap file PE dari dunia nyata.

## Build
Jalankan workflow **Android APK** melalui GitHub Actions. Artifact bernama `AsterPC-debug-apk`.

Target ABI: `arm64-v8a`.
