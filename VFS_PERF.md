# OpenBSD VFS performance — rozpoznanie

Baza: `upstream/master` @ 3dc8f332bbd (2026-08-08), branch `vfs-perf`.
Reguła projektu: **żadnej tezy bez dowodu** — albo linia kodu w drzewie, albo
wiadomość z listy z datą, albo liczba z pomiaru. Diffy mają być aplikowalne na
-current i przeżyć review deraadt@/kettenis@.

---

## 1. Kto jest właścicielem tej warstwy

Ustalone z historii commitów (`git log` na `sys/kern/vfs_*`, `sys/ufs`, `sys/uvm`)
oraz z ruchu na tech@. OpenBSD nie ma pliku MAINTAINERS — własność wynika z
faktycznej aktywności.

| Osoba | Obszar | Dowód |
|---|---|---|
| **mpi@** (Martin Pieuchot) | de facto właściciel odblokowywania VFS i UVM | 259 commitów w `sys/uvm` od 2020; seria "Push the KERNEL_LOCK() down…" I 2025; `Enable parallel fault handling on amd64 and arm64` (2025-12-01) |
| **beck@** (Bob Beck) | bufcache (`vfs_bio.c`), unveil w namei | najwięcej commitów w `vfs_bio.c`/`vfs_biomem.c` od 2020 |
| **kettenis@** (Mark Kettenis) | recenzent-hamulcowy; zgodność z POSIX | ubił wątek foffset() na POSIX 2.9.7 (2025-11-15) |
| **deraadt@** | polityka namei/unveil/pledge, `__pledge_open(2)` | intensywna praca w `vfs_lookup.c`/`pledge` III–VI 2026 |
| **claudio@** | `vnode_mtx`, blokowanie procesów/sygnałów | `Introduce a global vnode_mtx … safe to be called without the KERNEL_LOCK` (2021-04-28) |
| **mvs@** (Vitaliy Makkoveev) | odblokowywanie sysctl/socket; brał diff foffset | patrz wątek foffset; obecnie SysV IPC |
| **visa@, semarie@, anton@, guenther@** | higiena blokad vnode, `lseek(2)` NOLOCK (anton, 2021) | commity 2020–2022 |
| **kirill@** (Kirill A. Korinsky) | **nowa aktywna osoba w VFS w 2026** | `check vnode identity after vget` (2026-06-30), `discard buffers after vclean error`, `wake vclean after failed vnode lock attempts` (VI 2026) |
| **dlg@** | `struct mount` refcounty, `vfs_busy()` | 2025-01-02 |

Osoba, z którą trzeba uzgodnić kierunek przed napisaniem czegokolwiek: **mpi@**.
Recenzenci, którzy zablokują zły diff: **kettenis@** (semantyka POSIX),
**deraadt@** (namei/unveil/pledge).

---

## 2. Stan prac — co już zrobiono i gdzie utknęło

### 2.1 Ścieżka mpi@ (I–II 2025) — weszła do drzewa
- 2025-01-06 `Push the KERNEL_LOCK() down to vn_statfile() and unlock fstat(2)`
- 2025-01-20 `Push the KERNEL_LOCK() down to namei(9) in stat(2), lstat(2) & fstatat(2)`
- 2025-01-29 `Unlock open(2) & openat(2)` + `Move fd manipulation outside of the KERNEL_LOCK() scope in doopenat()`
- 2025-02-17 `Push KERNEL_LOCK() inside __realpath(2)`

Po 2025-02-17 **w drzewie nie ma ani jednego kolejnego commita odblokowującego
VFS** (stan na 2026-08-08). mpi@ przeszedł do UVM (X–XII 2025: parallel fault
handling, pdaemon). Front VFS stoi od ~18 miesięcy.

### 2.2 Ścieżka foffset() (X–XI 2025) — **odrzucona**
Wątek tech@ „Use foffset() to push the KERNEL_LOCK()", 2025-10-27 … 2025-11-16
(mpi@, mvs@, kettenis@). Pomysł: czytać `f_offset` przed wzięciem blokad, żeby
`KERNEL_LOCK()` objął tylko `VOP_READ`/`VOP_READDIR`.

Zabity przez kettenis@ (2025-11-15):
> „POSIX does make certain guarantees about atomicity of file operations
> (See 2.9.7 Thread Interactions with File Operations…). And while
> posix_getdents() isn't listed there, read() is."

Diff wycofano; przyznano, że wprowadzał błąd analogiczny do tego, który Linux
miał przed 3.14. **To jest twarda bramka review: każda zmiana dotykająca
`f_offset` w read/write musi respektować POSIX 2.9.7.**

### 2.3 Rekomendacja mpi@ po odrzuceniu (2025-11-16) — **wolny, nieobsadzony wątek**
mpi@ wskazał następny kierunek: zamiast `f_offset` zająć się **redukcją
contention na ścieżce `namei(9)`** — przyjrzeć się `vn_open()` i innym miejscom,
gdzie `namei(9)` nie jest jeszcze otoczone `KERNEL_LOCK()`.

Sprawdzone: **nikt tego nie podjął.** mvs@ po tym wątku poszedł w SysV IPC
(commity V–VII 2026). W drzewie jest 47 wywołań `namei()` w `sys/`, z tego 27 w
samym `vfs_syscalls.c`; `doopenat()` nadal bierze `KERNEL_LOCK()` wokół całego
`vn_open()` ([vfs_syscalls.c:1162](sys/kern/vfs_syscalls.c:1162)).

### 2.4 Analiza Mateusza Guzika (XII 2025) — twarde liczby, kontra-intuicyjne
Wątek tech@ „[POC] performance loss due to inefficiency of kernel", 2025-12-21.
Guzik to autor przepisania namecache i skalowania VFS we FreeBSD.

- Mikrobenchmark `fstat` w pętli, 8 procesów: 2 305 501 ops/s → **5 891 611 ops/s**
  (+155%) po zamianie ticket locka KERNEL_LOCK na lock Andersona. Diagnoza:
  ticket lock nie skaluje (scentralizowany odczyt) + false sharing (8 CPU na
  jednej linii cache przy 64 B).
- **Ale**: realny build jądra nie przyspieszył ani trochę.
- Profil `btrace` przy `make -j8`: dominuje **`pagezero`** (nietemporalne store'y
  w `uvm_pagealloc` — kontrproduktywne, gdy strona jest używana natychmiast),
  a po nim **`pageqlock`** (false sharing z `fpageqlock`, brak paddingu, statystyki).

Wniosek dla nas: **KERNEL_LOCK nie jest wąskim gardłem buildu.** Kto przyjdzie do
tech@ z „usuwamy giant lock, będzie szybciej" bez profilu, zostanie odesłany.

Wcześniejszy pomiar mpi@ („Analyse of kernel lock contention", 2021-09-06,
16-rdzeniowy ARM64): `make -j17` ≈ 40% czasu na spinowaniu — KERNEL_LOCK 18%,
SCHED_LOCK 10%, `pageqlock` 12%. Cztery lata później Guzik pokazuje przesunięcie
profilu — czyli **stare liczby są nieaktualne i trzeba je zmierzyć od nowa.**

### 2.5 Inne otwarte wątki
- „concerning vfs_stall_barrier()" — Guzik, 2025-09-13.
- „sys/ffs: reclaim vnode before dropping last ref" — kirill@/mvs@, VI 2026.
- bugs@: „Machine slows down to a crawl…" (Paul de Weerd, 2024-12-18) — jedyny
  świeży raport wydajnościowy o charakterze systemowym; wart odtworzenia.
- bugs@ poza tym: głównie panics/uvm_fault, nie wydajność. **Nie ma otwartego
  zgłoszenia „VFS jest wolny"** — to znaczy, że problem trzeba najpierw *pokazać*.

---

## 3. Wąskie gardła w kodzie (zweryfikowane w drzewie)

### F1. KERNEL_LOCK na ścieżce danych
`vn_read()`/`vn_write()` biorą `KERNEL_LOCK()` wokół VOP-ów
([vfs_vnops.c:355](sys/kern/vfs_vnops.c:355), [:399](sys/kern/vfs_vnops.c:399)),
tak samo `vn_statfile()`, `vn_ioctl()`, `vn_closefile()`, `vn_kqfilter()`.
W `vfs_syscalls.c` pozostały trzy miejsca: `doopenat()` (:1162), `dofstatat()`
(:2024), `sys___realpath()` (:897/:924).

### F2. Zero współdzielonych blokad vnode
- `sys/sys/lock.h:44` — `#define LK_SHARED RW_READ`; użyć w `sys/`: **0**.
  `LK_EXCLUSIVE`: 212 użyć w 52 plikach.
- `ufs_lock()` → `rrw_enter(&ip->i_lock, flags)` — prymityw *obsługuje* `RW_READ`.
- `mi_switch()` zwalnia cały kernel lock na czas snu
  ([sched_bsd.c:362](sys/kern/sched_bsd.c:362) `__mp_release_all`, przywraca
  na :443). Blokada vnode **nie jest** zwalniana.

Stąd teza: N czytelników tego samego pliku serializuje się na wyłącznej blokadzie
vnode przez cały czas oczekiwania na I/O w `bread()`, niezależnie od giant locka.
**Status: hipoteza strukturalnie poprawna, niezmierzona.** Nie wolno jej wnieść
na tech@ przed pomiarem.

Przeszkoda techniczna: `ffs_read()` ustawia `i_flag |= IN_ACCESS` — mutacja inode
pod blokadą dzieloną.

### F3. Namecache bez blokad, globalny LRU na ścieżce trafienia
[vfs_cache.c:44](sys/kern/vfs_cache.c:44): *"TODO: namecache access should really
be locked."* — jedyny taki TODO w VFS. Poważniejsze od braku blokady:
`cache_lookup()` przy każdym trafieniu robi `TAILQ_REMOVE`+`INSERT_TAIL` na
globalnej liście `nclruhead`. Globalny zapis na każdy komponent każdej ścieżki.

### F4. `vfs_bio.c` — zero mutexów/rwlocków, tylko `splbio()` (15 miejsc)

### F5. `getnewvnode()` — liniowy skan listy wolnych vnode z `VOP_ISLOCKED()` w pętli

### F6. `namei()` — `pool_get` 1 KB + `copyinstr` pełnej ścieżki na wywołanie

---

## 4. Plan

**Faza 0 — pomiar (obowiązkowa, bez skrótów).**
Sprzęt: nowoczesna maszyna 8+ rdzeni **oraz** Supermicro na Atomach (dużo słabych
rdzeni = najlepszy detektor serializacji; profil będzie inny niż na szybkim CPU —
to jest zaleta, nie problem).
Narzędzia: `btrace`/dt(4) — ta sama metodyka, co u Guzika, żeby liczby były
porównywalne; `will-it-scale` (Guzik używał PR #35).
Baseline musi objąć: `make -j N`, równoległy `read()` tego samego pliku,
równoległy `stat()`, równoległy `open()`+`close()`, obciążenie metadanymi.
Bez tego nie ruszamy kodu.

**Faza 1 — `namei(9)`.** Kierunek wskazany przez mpi@ 2025-11-16 i nieobsadzony.
Zaczynamy od inwentaryzacji 47 wywołań `namei()`: które są pod `KERNEL_LOCK`,
które nie, co konkretnie w środku wymaga jeszcze giant locka.
To jest jedyny wariant, w którym mamy błogosławieństwo właściciela warstwy.

**Faza 2 — F3 (namecache).** Naturalna kontynuacja fazy 1; własna blokada +
zdjęcie globalnego LRU z ścieżki trafienia.

**Faza 3 — F2 (`LK_SHARED`).** Dopiero z liczbami z fazy 0 na ręku. Wymaga
osobnej rozmowy z mpi@ — to nie jest kierunek, który wskazał.

**Faza 4 — F5/F6, potem F1/F4.**

### Zasady prowadzenia
1. Przed napisaniem kodu: mail do tech@ z profilem i pytaniem o kierunek,
   z jawnym odniesieniem do rekomendacji mpi@ z 2025-11-16.
2. Jeden diff = jedna zmiana. Styl OpenBSD (KNF). Bez refaktoryzacji „przy okazji".
3. Każdy diff dotykający read/write/getdents: jawna analiza POSIX 2.9.7.
4. Każdy diff dotykający namei: jawna analiza wpływu na unveil/pledge (deraadt@
   pisał 2025-02-12, że drobiazgowe blokowanie unveil „would make future work on
   vfs unlocking difficult").
5. Liczby przed/po z obu maszyn, metodyka `btrace` jak u Guzika.

## Status
- [x] Worktree `vfs-perf` na czystym upstream
- [x] Mapa właścicieli warstwy
- [x] Przegląd tech@, source-changes@, bugs@
- [ ] Faza 0 — środowisko testowe i baseline
