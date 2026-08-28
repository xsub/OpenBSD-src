# Dlaczego rodziny `statfs(2)` nie da się odblokować „tak samo jak reszty"

Analiza `sys_statfs()`, `sys_fstatfs()`, `sys_fhstatfs()` i `sys_getfsstat()`
w `sys/kern/vfs_syscalls.c`. Stan: `upstream/master` @ 3dc8f332bbd.

**Żadne z poniższych nie jest błędem w dzisiejszym jądrze.** Oba są blokerami
dla zdjęcia `KERNEL_LOCK()` i oba trzeba rozwiązać wcześniej.

---

## Bloker 1 — czas życia `struct mount`

`sys_statfs()`:
```c
	mp = nd.ni_vp->v_mount;
	sp = &mp->mnt_stat;
	vrele(nd.ni_vp);
	if ((error = VFS_STATFS(mp, sp, p)) != 0)
```
Referencja na vnode — jedyne, co trzyma ten mount przy życiu — jest zwalniana
**przed** użyciem `mp`. To samo w `sys_fhstatfs()` (`vput(vp)` przed
`VFS_STATFS`) i w `sys_fstatfs()` (`FRELE(fp, p)` przed `sp->f_flags = ...`).

`sys_getfsstat()` obok, w tym samym pliku, robi to poprawnie:
```c
	if (vfs_busy(mp, VB_READ|VB_NOWAIT))
		continue;
	...
	vfs_unbusy(mp);
```

### Dlaczego mimo to dziś nie ma wyścigu

Kuszące jest napisać „`vrele()` może zasnąć, więc unmount może w tym czasie
zwolnić `mp`". Prześledzone do końca — nie może:

1. `vrele()` zmniejsza `v_usecount` do zera, po czym bierze `vn_lock(vp,
   LK_EXCLUSIVE)` i woła `VOP_INACTIVE()`. Oba mogą zasnąć, a `mi_switch()`
   zwalnia wtedy cały kernel lock ([sched_bsd.c:362](sys/kern/sched_bsd.c:362)).
2. Ale w czasie tego snu **trzymamy blokadę vnode**. Równoległy `dounmount()`
   musi przejść przez `vflush()`, które chce zreklamować ten vnode i zablokuje
   się na tej samej blokadzie. Unmount nie kończy się więc podczas naszego snu.
3. `VOP_INACTIVE()` zwalnia blokadę vnode samodzielnie (`ufs_inactive()` kończy
   `VOP_UNLOCK`), ale po powrocie z niego **do samego `VFS_STATFS()` nie ma już
   ani jednego punktu snu** — `vputonfreelist()` działa pod `splbio()`.
   Kernel lock jest więc trzymany nieprzerwanie i wątek unmountujący na innym
   CPU nie ma jak wejść.

Czyli: bezpieczne, ale wyłącznie dzięki giant lockowi, i to w sposób, który
nie jest udokumentowany w kodzie. **Zdjęcie `KERNEL_LOCK()` z tej ścieżki
natychmiast otwiera use-after-free na `mp`.**

### Co trzeba zrobić najpierw

Wziąć `vfs_busy(mp, VB_READ|VB_WAIT)` przed zwolnieniem referencji na vnode,
wzorem `sys_getfsstat()`. Uwaga na kolejność blokad:

- `sys_statfs()` — `NDINIT()` bez `LOCKLEAF`, więc vnode jest **niezablokowany**;
  `vfs_busy()` przed `vrele()` jest bezpieczne i zgodne z kolejnością
  `mnt_lock` → blokada vnode, którą stosuje `dounmount()`.
- `sys_fstatfs()` — trzymamy tylko referencję na `struct file`, żadnej blokady
  vnode. Bezpieczne tak samo.
- `sys_fhstatfs()` — `VFS_FHTOVP()` zwraca vnode **zablokowany**. Wzięcie tu
  `mnt_lock` dałoby odwrócenie kolejności względem `dounmount()` i WITNESS by to
  zgłosił. Trzeba `vfs_mount_take(mp)` → `vput(vp)` → `vfs_busy()` →
  `vfs_mount_rele(mp)`.

---

## Bloker 2 — `mnt_stat` jest współdzielonym buforem roboczym

Wszystkie cztery syscalle przekazują do `VFS_STATFS()` wskaźnik na
`&mp->mnt_stat`, a implementacje piszą po nim w miejscu —
[ffs_statfs()](sys/ufs/ffs/ffs_vfsops.c) wypełnia `sbp->f_bsize`, `f_blocks`,
`f_bfree` … i woła `copy_statfs_info(sbp, mp)`.

Dwa równoległe `statfs(2)` na tym samym mouncie piszą więc po tej samej
strukturze. Dziś rozdziela je giant lock. **`vfs_busy(mp, VB_READ)` ich nie
rozdzieli** — to blokada dzielona, dwóch czytelników wejdzie równocześnie.

Dla FFS zapisy są prostoliniowe i bez snu, więc okno jest zerowe. Dla NFS nie:
[nfs_statfs()](sys/nfs/nfs_vfsops.c) woła `nfs_request()`, czyli synchroniczne
RPC, **i dopiero po nim** zapisuje `sbp`. Kernel lock jest w czasie RPC
zwolniony, więc dwa wątki mogą przeplatać się na `mnt_stat` już teraz. Skutek
jest praktycznie niegroźny (obie odpowiedzi opisują ten sam mount), ale to
prawdziwy data race i przy odblokowaniu przestaje być niegroźny.

### Co trzeba zrobić najpierw

Każdy wywołujący musi mieć własną `struct statfs`, zamiast pisać po
`mp->mnt_stat`. Rozmiar zmierzony na amd64: **568 bajtów** (dla porównania
`struct stat` to 128, `struct mount` to 696). To dużo jak na stos jądra
w liściu syscalla — decyzja stos vs `malloc(M_TEMP)` należy do maintainera.

---

## Wniosek

`statfs(2)` wygląda na kandydata z „grupy A", bo kształt funkcji jest równie
prosty jak `dopathconfat()`. Nie jest. Dwa niezależne blokery trzeba usunąć
wcześniej, a drugi z nich dotyka ABI wewnętrznego `VFS_STATFS()` i wszystkich
systemów plików.

Kolejność: (1) `vfs_busy()` w trzech funkcjach, (2) własny bufor u wywołujących,
(3) dopiero wtedy `NOLOCK`. Każde jako osobny diff.
