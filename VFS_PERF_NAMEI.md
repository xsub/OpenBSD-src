# Inwentaryzacja `namei(9)` vs `KERNEL_LOCK()`

Kierunek wskazany przez mpi@ na tech@ 2025-11-16 i do dziś nieobsadzony.
Stan drzewa: `upstream/master` @ 3dc8f332bbd.

## Wzorzec, który już wszedł

mpi@ przeprowadził w I–II 2025 cztery konwersje. Schemat jest zawsze ten sam
i widać go najlepiej w `dofstatat()` ([vfs_syscalls.c:2010](sys/kern/vfs_syscalls.c:2010)):

1. syscall dostaje `NOLOCK` w `syscalls.master`,
2. walidacja argumentów i `NDINITAT()` zostają **poza** blokadą,
3. `KERNEL_LOCK()` obejmuje wyłącznie `namei()` + pracę na vnode + `vput()`,
4. `copyout()`, `ktrace` i reszta epilogu wracają **poza** blokadę.

Zysk pojedynczej konwersji jest niewielki — sam mpi@ pisał o swojej serii
„this is not much in itself but is a requirement for upcoming changes".
Tak trzeba to opisywać na liście; obiecywanie przyspieszenia byłoby kłamstwem.

## Stan syscalli operujących na ścieżce

Z `syscalls.master`: `NOLOCK` mają dziś tylko `open`, `openat`, `stat`, `lstat`,
`fstat`, `fstatat`, `__realpath`, `lseek`, `read`, `write` i warianty pread/
pwrite/readv/writev. **Wszystkie pozostałe operacje na ścieżce biorą
`KERNEL_LOCK()` już na wejściu do syscalla.**

### Grupa A — read-only, jeden vnode, kształt identyczny z `dofstatat()`

| Funkcja | Ciało | Przeszkody |
|---|---|---|
| `dopathconfat()` :2090 | `namei()` + `VOP_PATHCONF()` + `vput()` | **brak** — najczystszy kandydat w całym pliku |
| `doreadlinkat()` :2141 | `namei()` + `VOP_READLINK()` + `vput()` | brak; `uiomove()` do userlandu zostaje pod blokadą, tak samo jak dziś |
| `dofaccessat()` :1924 | `namei()` + `VOP_ACCESS()` + `vn_writechk()` + `vput()` | podmienia `p->p_ucred` na czas wywołania (`crdup()`/`crfree()`) — patrz niżej |
| `sys_statfs()` :603 | `namei()` + `vrele()` + `VFS_STATFS()` | **tak** — patrz niżej |

#### `dofaccessat()`: podmiana `p_ucred`

Gdy `AT_EACCESS` nie jest ustawione, a real/effective id się różnią, funkcja
podstawia wątkowi tymczasowy `ucred` i przywraca oryginał na końcu. Dziś całość
jest pod giant lockiem. Przed zdjęciem blokady trzeba wykazać, że żaden inny
wątek nie czyta `p_ucred` cudzego `struct proc` bez synchronizacji. Wstępne
sprawdzenie: `kern_sysctl.c` odwołuje się do `ps_ucred` (per-proces) dla obcych
procesów, a `p_ucred` tylko dla `curproc` — ale to trzeba domknąć, zanim
cokolwiek się wyśle. **Nie zaczynać od tego syscalla.**

#### `sys_statfs()`: `mp` używany po `vrele()`

```c
	mp = nd.ni_vp->v_mount;
	sp = &mp->mnt_stat;
	vrele(nd.ni_vp);
	if ((error = VFS_STATFS(mp, sp, p)) != 0)
```
Referencja na vnode jest zwalniana **przed** użyciem `mp`. Dziś to bezpieczne
wyłącznie dlatego, że `KERNEL_LOCK()` nie dopuszcza równoległego `unmount`.
Zdjęcie blokady otwiera okno use-after-free na `mp`. Poprawka wymagałaby
`vfs_busy()`/refcountu (dlg@ dodał refcounty na `struct mount` 2025-01-02) —
czyli osobnego diffu, wcześniejszego niż jakiekolwiek odblokowanie.

### Grupa B — zapis metadanych na jednym vnode

`dofchmodat()` :2307, `dofchownat()` :2411, `dochflagsat()` :2205,
`doutimensat()` :2632, `sys_truncate()`.

Kształt: `namei()` → `vn_lock(LK_EXCLUSIVE)` → `VOP_SETATTR()` → `vput()`.
Strukturalnie równie proste jak grupa A, ale każdy dotyka uprawnień i pledge,
więc każdy potrzebuje osobnego uzasadnienia. Po grupie A, nie wcześniej.

### Grupa C — mutacje katalogu

`dolinkat()` :1652, `dosymlinkat()` :1733, `dounlinkat()` :1794,
`domknodat()` :1519, `sys_rename()`, `sys_mkdir()`, `sys_rmdir()`.

Te używają `LOCKPARENT`, trzymają jednocześnie rodzica i dziecko, mają ścieżki
`VOP_ABORTOP()` i w `rename` dwa niezależne `namei()`. Kolejność blokad jest tu
nietrywialna. **Nie ruszać**, dopóki grupy A i B nie są w drzewie i przetestowane.

### Grupa D — poza zakresem

`sys_mount()`, `sys_unmount()`, `sys_quotactl()`, `sys_getfh()`, `sys_fhopen()`,
`sys_unveil()`. Rzadkie, uprzywilejowane, o dużym ciężarze bezpieczeństwa.
Nie ma tu nic do ugrania.

## Kolejność prac

1. `dopathconfat()` — zerowe ryzyko, ustala wzorzec diffu do recenzji.
2. `doreadlinkat()` — realnie gorący (`ld.so`, każdy `readlink` w skrypcie).
3. `struct mount` w `sys_statfs()` — najpierw naprawić lifetime, potem odblokować.
4. `dofaccessat()` — dopiero po domknięciu sprawy `p_ucred`.
5. Grupa B.

## Czego ta lista nie rozstrzyga

Nie wiemy, czy którykolwiek z tych syscalli jest wąskim gardłem u kogokolwiek.
Pomiar Guzika z XII 2025 pokazał, że przy `make -j8` dominuje `pagezero`
i `pageqlock`, a nie giant lock. Te konwersje uzasadnia **struktura**, nie
wydajność — i tak trzeba je przedstawiać.
