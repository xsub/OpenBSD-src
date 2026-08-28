# vfslab — narzędzia pomiarowe do prac nad wydajnością VFS

**Nie jest to materiał do wysłania na tech@.** Patche jądra na tym branchu są
osobno i mają się bronić same; ten katalog to obudowa pomiarowa, która żyje
tylko u nas.

Zasada: **nie dodajemy do jądra instrumentacji, którą da się uzyskać
istniejącymi narzędziami.** Poniższa tabela mówi, co czego wymaga.

| Narzędzie | Wymaga | Uzasadnienie |
|---|---|---|
| `bin/nchstats-delta.sh` | **nic** | `kern.nchstats` jest w GENERIC i eksportowane przez `sysctl(2)` ([kern_sysctl.c:765](../sys/kern/kern_sysctl.c:765)) |
| `btrace/namei.bt` | `option DDBPROF` (amd64/i386) | `namei()` to zwykła funkcja globalna — wystarczy provider `kprobe` |
| `btrace/vnode-lock.bt` | patch `rwlock:vnode` z tego brancha | jedyny przypadek, w którym samo dt(4) nie wystarcza — patrz niżej |

## Dlaczego blokady vnode wymagają patcha, a reszta nie

`kprobe` potrafi obłożyć `rw_enter()`, ale ta funkcja obsługuje **wszystkie**
rwlocki w systemie. Nie ma z niej sposobu odfiltrowania blokad vnode bez
sięgania po `rwl_name`, którego kprobe nie widzi.

Statyczne probe'y `rwlock` są indeksowane (`rwl_traceidx`) właśnie po to, żeby
dało się nazwać konkretną klasę blokad — tak działają już `rwlock:netlock`
i `rwlock:solock`. Blokadom vnode brakowało wyłącznie indeksu. Stąd patche:

```
rwlock: pass `flags' through rrw_init_flags() in the !WITNESS case.
rwlock: let rrwlocks carry a dt(4) trace index.
dt: add an rwlock:vnode static probe.
ffs: fire the rwlock:vnode probe for inode locks.
Fire the rwlock:vnode probe for the remaining filesystems.
```

## Czego te narzędzia *nie* mierzą

- **Czasu trzymania blokady.** `TRACEINDEX` w `kern_rwlock.c` siedzi wyłącznie
  na ścieżkach wejścia i na `rw_upgrade()`; `rw_exit()` nie ma probe'a. Dostajemy
  „ile razy przejęcie blokady wymagało snu", a nie „jak długo była trzymana".
  To wystarcza do zlokalizowania contention i nie wystarcza do jego wyceny.
- **Rekurencyjnych przejęć.** `rrw_enter()` ma szybką ścieżkę dla właściciela,
  która nie dotyka `rw_enter()`. Takie przejęcia nie generują zdarzeń — słusznie,
  bo nie mogą się z niczym ścigać.
- **Zajętości namecache.** `numcache` i `numneg` nie są nigdzie eksportowane,
  więc nie odróżnimy chybienia „bo zimne" od „bo wyeksmitowane".

## Kolejność użycia

1. `nchstats-delta.sh` na obciążeniu referencyjnym — czy w ogóle mamy problem
   z namecache. Zero kosztu, zero zmian w jądrze.
2. `namei.bt` — rozkład kosztu pojedynczego rozwiązania ścieżki.
3. `vnode-lock.bt` — dopiero gdy punkty 1–2 wskażą, że warto.
