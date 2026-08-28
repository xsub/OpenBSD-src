# OpenBSD VFS performance — analiza wąskich gardeł

Baza: `upstream/master` @ 3dc8f332bbd (2026-08-08), branch `vfs-perf`.
Cel: zidentyfikować i usunąć wąskie gardła warstwy VFS. Ścieżka docelowa —
diffy aplikowalne na -current, do przeglądu przez developera OpenBSD.

## Mapa ustaleń (pierwszy przebieg, statyczny)

### F1. KERNEL_LOCK obejmuje całą ścieżkę danych
`sys/kern/vfs_vnops.c`:
- `vn_read()` :355 — `KERNEL_LOCK()` wokół `VOP_READ`
- `vn_write()` :399 — `KERNEL_LOCK()` wokół `VOP_WRITE`
- `vn_statfile()` :519, `vn_ioctl()` :613, `vn_closefile()` :631, `vn_kqfilter()` :662
- `vn_seek()` — `KERNEL_LOCK` tylko wokół `VOP_GETATTR` (SEEK_END)

`sys/kern/syscalls.master` oznacza read/write/pread/pwrite/readv/writev/open/
openat/close/lseek/stat/lstat/fstat/fstatat jako `NOLOCK`, ale giant lock i tak
jest brany od razu w warstwie vnode. Zysk z `NOLOCK` jest więc pozorny dla FS.

Usunięcie F1 wymaga MP-safe FFS + bufcache + UVM — to projekt wieloletni,
nie pierwszy krok.

### F2. Brak współdzielonych blokad vnode  ← najlepszy pierwszy cel
- `sys/sys/lock.h:44` — `#define LK_SHARED RW_READ`
- Liczba użyć `LK_SHARED` w całym `sys/`: **0**
- Liczba użyć `LK_EXCLUSIVE`: 212 w 52 plikach
- `ufs_lock()` (`sys/ufs/ufs/ufs_vnops.c:1475`) → `rrw_enter(&ip->i_lock, flags)`,
  czyli prymityw *już* obsługuje `RW_READ`

Konsekwencja: `vn_read()` bierze `LK_EXCLUSIVE`. `ffs_read()` trzyma tę blokadę
przez cały `bread()`, a `bread()` śpi. KERNEL_LOCK jest zwalniany przy
przełączeniu kontekstu — blokada vnode **nie**. Czyli N wątków czytających ten
sam plik serializuje się na pełnym czasie oczekiwania na I/O, niezależnie od
giant locka. To wąskie gardło mierzalne dziś, bez ruszania F1.

Przeszkoda do rozwiązania: `ffs_read()` ustawia `i_flag |= IN_ACCESS` —
mutacja inode pod blokadą dzieloną. Wymaga atomowej aktualizacji flagi.

### F3. Namecache bez blokad + globalny LRU na ścieżce trafienia
`sys/kern/vfs_cache.c:45` — komentarz w drzewie: *"TODO: namecache access should
really be locked."* Chroniony wyłącznie przez KERNEL_LOCK.

Gorszy problem niż brak blokady: `cache_lookup()` przy **każdym trafieniu** robi
`TAILQ_REMOVE` + `TAILQ_INSERT_TAIL` na globalnej liście `nclruhead`
(i `nclruneghead` dla wpisów negatywnych). To globalny zapis na każdy komponent
każdej ścieżki w systemie — ping-pong linii cache nawet po zdjęciu giant locka.

### F4. Bufcache polega wyłącznie na KERNEL_LOCK
`sys/kern/vfs_bio.c` — zero `struct mutex` / `struct rwlock`; tylko `splbio()`
(15 miejsc). Brak własnej synchronizacji MP.

### F5. `getnewvnode()` — liniowy skan listy wolnych vnode
`sys/kern/vfs_subr.c` — `TAILQ_FOREACH(vp, listhd, v_freelist)` z
`VOP_ISLOCKED(vp)` w środku, pod `splbio()`. Komentarz przyznaje, że opiera się
na założeniu "co najwyżej pierwsze NCPUS elementów jest zablokowanych".

### F6. `namei()` — koszt stały na wywołanie
`pool_get(&namei_pool)` (MAXPATHLEN = 1024 B) + `copyinstr()` pełnej ścieżki,
plus `unveil_check_component()` per komponent, plus `pledge_namei()`.

## Plan fazowy

- **Faza 0 — pomiar.** Zestaw mikro/makro benchmarków (równoległy read tego
  samego pliku, równoległy stat, build-jak-obciążenie), baseline na wielordzeniowej
  maszynie. Profilowanie: `dt(4)`/`btrace`, `systat`, `WITNESS`.
- **Faza 1 — F2:** `LK_SHARED` dla `VOP_READ`/`VOP_GETATTR` na FFS. Najwięcej
  zysku na jednostkę ryzyka, izolowane, mierzalne przed usunięciem giant locka.
- **Faza 2 — F3:** własna blokada namecache + eliminacja globalnego LRU z
  ścieżki trafienia (np. licznik/clock zamiast LRU, albo LRU per-CPU).
- **Faza 3 — F5/F6:** tanie, lokalne poprawki alokacji vnode i namei.
- **Faza 4 — F1/F4:** dopiero po powyższym, i tylko odcinkami.

Każda faza: patch minimalny, w stylu OpenBSD, z liczbami przed/po.

## Status
- [x] Worktree `vfs-perf` na czystym upstream
- [ ] Faza 0 — środowisko testowe i baseline
