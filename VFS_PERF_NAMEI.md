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

| Funkcja | Stan | Uwagi |
|---|---|---|
| `dopathconfat()` :2090 | **zrobione** | `namei()` + `VOP_PATHCONF()` + `vput()`, nic więcej |
| `doreadlinkat()` :2141 | **zrobione** | `uiomove()` do userlandu zostaje pod blokadą, wewnątrz `VOP_READLINK()`, tak jak było |
| `dofaccessat()` :1924 | **zrobione** | podmiana `p_ucred` przeanalizowana — patrz niżej |
| `sys_statfs()` :603 | **zablokowane** | dwa niezależne blokery, patrz [VFS_PERF_STATFS.md](VFS_PERF_STATFS.md) |
| `sys_chdir()` :771, `sys_chroot()` :797 | **zablokowane** | `fd_cdir`/`fd_rdir` chronione giant lockiem, patrz niżej |

#### `dofaccessat()`: podmiana `p_ucred` — rozstrzygnięte, bezpieczne

Gdy `AT_EACCESS` nie jest ustawione, a real/effective id się różnią, funkcja
podstawia wątkowi tymczasowy `ucred`. Przeszukane całe `sys/`: **każdy odczyt
i zapis `p_ucred` idzie przez wątek będący jego właścicielem.** Obce procesy
sięga się przez `ps_ucred`:

- `cansignal(struct proc *p, struct process *qr, ...)` — `p` to wysyłający,
  cel przez `qr->ps_ucred`.
- `ktrcanset(struct proc *callp, struct process *targetpr)` — tak samo.
- `kern_sysctl.c` porównuje `cp->p_ucred` wołającego z `findpr->ps_ucred` celu.
- Wszystkie przypisania do `p_ucred` (`subr_prof.c`, `kern_exec.c`,
  `kern_sig.c` w coredumpie, `init_main.c` dla proc0) dotyczą własnego wątku.

Do tego `crdup()`/`crfree()` są już MP-safe: `refcnt_take()`/`refcnt_rele()`
na atomowym liczniku, pod spodem `pool_get()`/`pool_put()`.

#### `sys_chdir()` / `sys_chroot()`: `fd_cdir` i `fd_rdir` bez własnej blokady

`sys_chdir()` po `change_dir()` robi podmianę:
```c
	old_cdir = fdp->fd_cdir;
	fdp->fd_cdir = nd.ni_vp;
	vrele(old_cdir);
```
`namei()` czyta `fdp->fd_cdir` i robi na nim `vref()` — bez żadnej blokady
([vfs_lookup.c](sys/kern/vfs_lookup.c)). W procesie wielowątkowym wątek robiący
`chdir()` ściga się więc z wątkiem robiącym `namei()`. Czyta to również
`kern_sysctl.c:2398` dla **obcego** procesu (`findpr->ps_fd->fd_cdir`).

Drzewo samo to przyznaje — [uipc_usrreq.c:1071](sys/kern/uipc_usrreq.c:1071):
```c
	/* fdp->fd_rdir requires KERNEL_LOCK() */
```

Czyli `fd_cdir`/`fd_rdir` są dziś chronione wyłącznie giant lockiem. Zanim
`chdir(2)`, `chroot(2)` czy `__getcwd(2)` da się odblokować, trzeba im dać
własną synchronizację. To osobny projekt, nie przypis do tej pracy.

### Grupa B — zapis metadanych na jednym vnode: **świadomie odłożone**

`dofchmodat()` :2307, `dofchownat()` :2411, `dochflagsat()` :2205,
`doutimensat()` :2632, `sys_truncate()`.

Kształt jest równie prosty jak w grupie A (`namei()` → `vn_lock(LK_EXCLUSIVE)`
→ `VOP_SETATTR()` → `vput()`), ale:

1. Żaden z tych syscalli nie jest gorący. mpi@ w swojej serii też ich nie
   ruszył, choć były równie łatwe — to sygnał, nie przeoczenie.
2. `dochflagsat()` i `doutimensat()` delegują do `dovchflags()` i `dovutimens()`,
   dzielonych ze ścieżkami po deskryptorze (`sys_fchflags()`, `sys_futimens()`).
   Blokada musiałaby objąć wywołanie helpera, a nie jego wnętrze, żeby nie
   zagnieżdżać jej na ścieżce fd. Osobne uzasadnienie na syscall.
3. Każdy dotyka uprawnień i pledge, więc każdy to osobna dyskusja na liście.

Stosunek zysku do kosztu recenzji jest zły. Wracamy tu dopiero, gdy grupa A
będzie w drzewie.

### Poza grupami — `getdents(2)`: **zrobione**

Nie używa `namei()`, ale to najgorętszy pozostały konsument giant locka na
ścieżce metadanych (każde `ls`, `find`, każda pętla `readdir`). Region blokady
zawężony do samego `VOP_READDIR()`, wzorem `vn_seek()`. Obsługa `f_offset`
nietknięta, więc zarzut o POSIX 2.9.7, który zatopił próbę z `foffset()`,
tu nie występuje.

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

## Kolejność prac — stan

Zrobione: `pathconf`/`pathconfat`, `readlink`/`readlinkat`, `access`/`faccessat`,
`getdents`.

Zablokowane i udokumentowane: `statfs` (czas życia `struct mount` + współdzielony
`mnt_stat`), `chdir`/`chroot` (`fd_cdir`/`fd_rdir` bez własnej blokady).

Odłożone świadomie: cała grupa B.

Nie ruszane: grupa C (mutacje katalogu) i D.

## Czego ta lista nie rozstrzyga

Nie wiemy, czy którykolwiek z tych syscalli jest wąskim gardłem u kogokolwiek.
Pomiar Guzika z XII 2025 pokazał, że przy `make -j8` dominuje `pagezero`
i `pageqlock`, a nie giant lock. Te konwersje uzasadnia **struktura**, nie
wydajność — i tak trzeba je przedstawiać.
