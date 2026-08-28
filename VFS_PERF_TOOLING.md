# Ocena planu `vfsprof` + `vfsbench`

Weryfikacja względem drzewa `upstream/master` @ 3dc8f332bbd. Każdy zarzut ma
odnośnik do linii kodu albo do wiadomości z listy.

---

## Werdykt w jednym zdaniu

**`vfsbench` — tak, prawie w całości. `vfsprof` — nie w tej formie:** duplikuje
dt(4)/btrace, których OpenBSD już ma, i wprowadza nowe ABI sysctl, którego nikt
nie przyjmie. Instrumentacja, której plan potrzebuje, jest w drzewie w 90%
gotowa — brakuje kilkudziesięciu linii, nie nowego podsystemu.

---

## 1. Co zabija `vfsprof` w obecnej postaci

### 1.1 dt(4) już to robi
W drzewie jest `sys/dev/dt/` z czterema providerami: `static`, `kprobe`,
`profile`, `syscall`. **Istnieje już provider `vfs`** —
[dt_prov_static.c](sys/dev/dt/dt_prov_static.c):
```
DT_STATIC_PROBE3(vfs, bufcache_rel, "long", "int", "int64_t");
DT_STATIC_PROBE3(vfs, bufcache_take, "long", "int", "int64_t");
DT_STATIC_PROBE4(vfs, cleaner, "long", "int", "long", "long");
```
Dodanie punktu pomiarowego to jedna linia `TRACEPOINT(vfs, nazwa, args)`
([sys/sys/tracepoint.h:28](sys/sys/tracepoint.h:28)) plus wpis w tablicy.

Precedens rozstrzygający: **Mateusz Guzik zdobył swoje liczby (XII 2025) samym
`btrace`, bez jednego patcha do jądra.** Przyjście na tech@ z własnym
podsystemem licznikowym po tym, jak ktoś zrobił to samo istniejącym narzędziem,
kończy się jednym zdaniem od deraadt@.

### 1.2 Nowe ABI sysctl nie przejdzie
`struct vfsprof_snapshot` z polami `version`, `size`, `generation` to idiom
Linuksa/Solarisa. OpenBSD nie utrzymuje wersjonowanych struktur eksportowych dla
narzędzi diagnostycznych — od tego jest dt(4), które ma już swój interfejs
(`/dev/dt`, `btrace(8)`). Dodatkowo `CTL_VFS` jest celowo mikroskopijny —
[mount.h:449-461](sys/sys/mount.h:449) to `VFS_GENERIC`, `VFS_MAXTYPENUM`,
`VFS_CONF` i nic więcej.

### 1.3 Maski profilowania to funkcja dt(4)
`VFSPROF_NAMEI | VFSPROF_NAMECACHE | …` — dt(4) włącza i wyłącza pojedyncze
probe'y z userlandu. To jest dokładnie ta sama funkcjonalność, napisana drugi raz.

### 1.4 Liczniki per-CPU już istnieją
Plan proponuje własny mechanizm. W drzewie jest
[sys/sys/percpu.h](sys/sys/percpu.h) — `cpumem`, `counters_alloc()`,
`counters_inc()`, `counters_read()` (dlg@, używane przez `uipc_mbuf.c`,
`subr_evcount.c`). Pisanie własnego to gwarantowany komentarz w review.

### 1.5 `affinity.c` jest niewykonalny
**OpenBSD nie ma userlandowego API do przypinania wątków do CPU.** Sprawdzone:
w `syscalls.master` nie ma `cpuset`, `sched_setaffinity` ani odpowiednika.
Ten plik trzeba usunąć z planu, a matrycę skalowania projektować bez
przypinania — co swoją drogą oznacza, że wyniki będą zaszumione przez scheduler
i tym bardziej potrzebują powtórzeń i histogramów.

### 1.6 tmpfs nie jest w GENERIC
[sys/conf/GENERIC:42](sys/conf/GENERIC:42): `#option TMPFS` — zakomentowane.
Kod jest w `sys/tmpfs/`, ale wymaga własnego jądra i jest słabo utrzymywany.
Porównanie FFS/tmpfs zostanie na tech@ podważone („tmpfs nie jest w GENERIC, więc
co ta liczba znaczy"). Trzymać jako narzędzie diagnostyczne, nie jako argument.

---

## 2. Co zastępuje `vfsprof` — konkretny pierwszy patch

`kern_rwlock.c` **jest już w pełni oinstrumentowany**: 12 wywołań
`TRACEINDEX(rwlock, rwl->rwl_traceidx, …)` na każdym wejściu, oczekiwaniu,
przejęciu i zwolnieniu ([kern_rwlock.c:240-471](sys/kern/kern_rwlock.c:240)).

`rrw_enter()` przechodzi przez `rw_enter()` — czyli blokada inode **już przepływa
przez tę instrumentację**. Brakuje jej wyłącznie indeksu trace:

- [rwlock.h:245](sys/sys/rwlock.h:245): `/* sorted alphabetically, keep in sync
  with dev/dt/dt_prov_static.c */` — zarejestrowane są tylko
  `DT_RWLOCK_IDX_NETLOCK` i `DT_RWLOCK_IDX_SOLOCK`.
- [rwlock.h:205](sys/sys/rwlock.h:205): `_rrw_init_flags()` nie ma wariantu
  `_trace`, w odróżnieniu od `rw_init_flags_trace()` (:144).
- [ffs_vfsops.c:1216](sys/ufs/ffs/ffs_vfsops.c:1216) i
  [ext2fs_vfsops.c:857](sys/ufs/ext2fs/ext2fs_vfsops.c:857):
  `rrw_init_flags(&ip->i_lock, "inode", RWL_DUPOK | RWL_IS_VNODE);`

**Patch:** `DT_RWLOCK_IDX_VNODE 3` + wpis `DT_STATIC_PROBE3(rwlock, vnode, …)` +
`rrw_init_flags_trace()` + zmiana dwóch call site'ów. Rząd wielkości: 30 linii.

Efekt: pełne dane o contention na blokadach vnode przez `btrace`, bez nowego ABI,
bez nowych liczników, bez `option VFSPROF`. To jest **cały Stage C planu**
załatwiony patchem, który ma realną szansę przejść upstream sam z siebie.

Analogicznie Stage A/B: `TRACEPOINT(vfs, namei_enter/…)` w `vfs_lookup.c` i
`TRACEPOINT(vfs, cache_hit/miss/neghit)` w `vfs_cache.c` — po jednej linii,
w istniejącym providerze `vfs`.

---

## 3. Co w planie jest dobre i zostaje

| Element | Dlaczego zostaje |
|---|---|
| **Matryca shared-dir/private-dir × shared-files/private-files** (§16–17) | Najlepszy pomysł w całym dokumencie. Jeśli skalowanie wali się tylko przy `--shared-dir`, contention jest zlokalizowany bez zgadywania. |
| **Negative lookup jako workload pierwszej klasy** (§13) | OpenBSD trzyma osobną listę LRU wpisów negatywnych (`nclruneghead` w [vfs_cache.c](sys/kern/vfs_cache.c)); ten benchmark mapuje się 1:1 na istniejący mechanizm. |
| **Scaling efficiency `T(N)/(N·T(1))`** (§19) | Właściwa metryka. Sam throughput niczego nie dowodzi. |
| **Histogramy logarytmiczne, p99/p99.9** (§22) | Convoying na blokadach widać w ogonie, nie w medianie. Przy braku affinity — konieczne. |
| **Liczniki lokalne w workerach, agregacja po stopie** (§24) | Ta sama zasada co w jądrze. Poprawna. |
| **Bariera startu** (§25) | Krytyczna dla krótkich przebiegów SMP. |
| **Deterministyczny generator z seed + manifest** (§26) | Powtarzalność. |
| **Test narzutu instrumentacji** (§30) | Obowiązkowy. W wersji z dt(4) sprowadza się do „probe wyłączony vs włączony". |
| **Bramka optymalizacji, 8 kroków** (§35) | To jest najmocniejsza część dokumentu i dokładnie ta dyscyplina, która przechodzi przez tech@. Zostaje bez zmian. |
| **Ramowanie „evidence first, patches second"** (§37) | Zgodne z tym, co pokazuje historia: mpi@ publikował profile zanim ruszył kod. |

---

## 4. Poprawki do matrycy eksperymentu (§36)

288 przebiegów × 5 powtórzeń ≈ 1440 obserwacji — na 8-rdzeniowej maszynie to
realne, na Supermicro na Atomach nie. Propozycja: pełna matryca na Atomach tylko
dla `lookup` i `stat`, reszta na szybkiej maszynie. Atomy są cenniejsze jako
detektor serializacji (dużo słabych rdzeni), nie jako źródło liczb bezwzględnych.

Do matrycy dodać brakującą oś: **`--depth` × `--shared-dir`**. Głębokość ścieżki
mnoży liczbę komponentów, a każdy komponent to jedno `cache_lookup()` z globalnym
zapisem na `nclruhead` ([vfs_cache.c](sys/kern/vfs_cache.c)) — to jest dokładnie
ten efekt, który chcemy zobaczyć.

---

## 5. Skorygowana struktura repo

```
openbsd-vfs-lab/
├── kernel/patches/
│   ├── 0001-dt-rwlock-vnode-probe.diff    # ~30 linii, kandydat do upstream
│   ├── 0002-dt-vfs-namei-tracepoints.diff
│   └── 0003-dt-vfs-namecache-tracepoints.diff
├── vfsbench/                               # bez zmian względem planu
├── workloads/                              # bez affinity.c
├── btrace/                                 # skrypty .bt zamiast vfsprof
│   ├── vnode-lock-contention.bt
│   ├── namei-latency.bt
│   └── namecache-hitrate.bt
└── scripts/
```

Zniknęło: `include/vfsprof.h`, `0001-vfsprof-core.diff`, `affinity.c`.
Doszło: katalog skryptów `btrace`.
