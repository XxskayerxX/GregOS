# GregOS — Feuille de route ultime

> **Vision.** Faire de GregOS un système d'exploitation *complet*, *isolé*, *sûr* et
> *optimisé* : un vrai noyau multitâche préemptif, un espace utilisateur riche capable
> de se recompiler lui-même (self-hosting), un WindowServer en Ring 3, une pile réseau
> de production, un système de fichiers persistant robuste, et le tout tournant en
> 64 bits multi-cœurs. Ce fichier est **la seule feuille de route** du projet : il
> décrit l'état atteint, les limites structurelles actuelles, et le chemin détaillé —
> chantier par chantier — vers « le meilleur OS du monde ».
>
> *Seigneur du Kernel — que le Drakkar avance.*

---

## Partie I — État des lieux

### I.1 Ce qui est DÉJÀ fait (socle solide)

GregOS n'est pas un jouet : c'est déjà un OS bootable, graphique et multitâche.

| Domaine | Acquis |
|---|---|
| **Boot** | GRUB2 Multiboot2, VBE 800×600×32bpp, fallback VGA texte |
| **Langage** | C freestanding + C++11 (`-ffreestanding -fno-exceptions -fno-rtti`), **0 warning** |
| **Bibliothèque** | `Greg::` (AK-inspired) complète : String, Vector, HashMap, RefPtr/OwnPtr, Function, Optional, ErrorOr, Span, CircularBuffer, StringBuilder |
| **Mémoire** | Allocateur free-list (`kmalloc`/`kfree` avec coalescing), heap 32 Mo, `Kernel::MemoryManager` |
| **Paging** | PSE 4 Mo, identity-map 0–48 Mo + **répertoires de pages par processus** (clonés, CR3 commuté au context-switch) |
| **Ordonnanceur** | Round-robin **préemptif** (IRQ0 100 Hz), `switch_task`, `Thread{cr3}` |
| **Ring 3** | Vraie isolation : GDT user, TSS, `INT 0x80` (14 syscalls), ELF32 `ET_EXEC`, argv/argc, heap par processus, **récupération de faute Ring-3** (un process fautif est tué, le noyau survit) |
| **Fichiers** | VFS en mémoire (64 fichiers × 4096 o), persistance disque **ATA PIO** (format `GRFS`), lu/écrit **depuis l'espace utilisateur isolé** (SYS_OPEN/READ/CREATE/WRITE_FILE) |
| **Réseau** | Pile TCP/IP sur **RTL8139** : Ethernet/ARP/IPv4/ICMP/UDP/TCP-client/DNS/DHCP/HTTP-1.0 (`ping`, `nslookup`, `curl`, `wget` réels) |
| **GUI** | Compositor, WindowManager (déplacement/resize/min/max/Alt-Tab), ~15 applications fenêtrées (Terminal, Fichiers, Paint, Horloge, Calc, Navigateur, Casino, 10 jeux…) |
| **Périphériques** | PS/2 clavier + souris, PIT 100 Hz, haut-parleur PC, RTC |

### I.2 Limites structurelles à lever (le « pourquoi » de cette feuille de route)

Ces plafonds sont les **vraies** frontières entre « démo impressionnante » et « OS de
production ». Chaque chantier ci-dessous en supprime un ou plusieurs.

| # | Limite actuelle | Localisation | Conséquence |
|---|---|---|---|
| L1 | **32 bits, mono-CPU** | `Makefile -m32`, pas d'APIC | Pas de SMP, ≤ 4 Go, ISA vieillissante |
| L2 | **Pools de processus statiques** `VM_MAX_PROC = 6` | `kernel.c:862` | Max 6 process isolés simultanés, pas de vrai allocateur de frames |
| ~~L3~~ | ~~**Threads zombies non recyclés**~~ ✅ **corrigé (Phase 5.1)** | `Scheduler::reap_zombies()` | Slots + stacks + VM slot recyclés à l'`exit` ; `run`/`ring3` illimités |
| L4 | **VFS plat** : 64 fichiers × ~~4~~ **8 Ko** max 🟡 | `kernel.c:19-21` | Taille/fichier relevée à 8 Ko (2026-07-08) ; reste : arborescence réelle, gros fichiers, FS on-disk normalisé |
| ~~L5~~ | ~~**« fd » = id d'entrée VFS**~~ ✅ **corrigé (Phase 5.4)** | `fd_tab[]` (kernel.c) | fd = petit entier par processus, offset par-fd, `SYS_LSEEK` ; reste `stat`/`dup` |
| L6 | **Pas de `fork()`** | — | Modèle de process incomplet, pas de shell/pipeline Unix |
| ~~L7~~ | ~~**Réseau *polling*** (pas d'IRQ RX)~~ ✅ **corrigé (Phase 9.1, 2026-07-08)** | `net.c` | RX piloté par IRQ + file SPSC + `hlt` dans les attentes ; reste : serveur TCP, TLS (Phase 9.3/9.4) |
| L8 | **ATA PIO** (pas de DMA) | `drivers/ata.c` | I/O disque lentes, pas d'AHCI/USB/CD |
| L9 | **GUI dans le noyau (Ring 0)** | `kernel/Compositor.cpp` | Un bug GUI = panique noyau ; pas de vrai WindowServer |
| L10 | **Sécurité minimale** : mdp « admin » en dur, pas de permissions 🟡 | `LoginWindow`, VFS | Validation des pointeurs syscall FAITE (Phase 14.2) ; reste : utilisateurs, sandboxing, W^X/ASLR |
| ~~L11~~ | ~~**Arrêt câblé QEMU** (`0x604/0xB004`)~~ ✅ **corrigé (2026-07-08)** | `drivers/acpi.c` | Arrêt S5 ACPI réel (RSDP→RSDT→FADT→DSDT `_S5` parsés) ; reboot via ResetReg ACPI 2.0+ avec fallback PS/2 |
| L12 | **Pas de libc / pas d'édition de liens dynamique** | — | Chaque programme est un ELF statique bricolé ; pas d'écosystème |

---

## Partie II — La feuille de route (chantiers)

> **Légende.** Effort : ⏱️S (jours) · ⏱️M (semaines) · ⏱️L (mois). Risque : 🟢 faible ·
> 🟡 moyen · 🔴 élevé. Chaque tâche liste : *objectif*, *implémentation (ancrée dans le
> code GregOS)*, *critère d'acceptation (vérifiable en QEMU)*.

---

### Phase 5 — Modèle de processus complet ⏱️M 🟡

*Finir ce qui est commencé : passer d'« exécuter un ELF isolé » à un vrai modèle Unix.*

**5.1 — Recyclage des processus (corrige L3). ⏱️S 🟢 — ✅ FAIT (2026-07-08)**
- *Objectif :* un thread qui `exit` libère son slot, sa `cr3`, ses stacks.
- *Impl (livrée) :* état `ThreadState::Zombie` ajouté ; `sys_exit`/`kill_current`
  marquent le thread `Zombie` puis `yield`. `Scheduler::reap_zombies()` (appelé en tête
  de `tick()`) récupère chaque slot `Zombie` ≠ thread courant : `kfree` des stacks
  noyau/user (champs `kstack_alloc`/`ustack_alloc`), `vm_release_cr3()` (kernel.c) qui
  remet `proc_used[slot]=0` + `proc_heapnext[slot]=0`, puis remet le slot à `Invalid`
  pour réemploi. Le reaper ne touche jamais la pile du thread courant (réappé au tick
  suivant depuis un autre contexte).
- *Acceptation : ✅ vérifiée en QEMU.* `run hello` ×13 et `ring3` ×15 (> 2× l'ancien
  plafond de 6) : le `pid` reste à 2 (slot réutilisé au lieu de grimper puis saturer),
  `SYS_MMAP` rend toujours `VA=0x50000000`, lecture/écriture de fichiers OK, shell vivant.
- *Reste (→ 5.2) :* stocker le code retour et réveiller le parent (`wait()`), passer la
  table de processus statique en dynamique.

**5.2 — Table de processus dynamique (corrige L2). ⏱️M 🟡**
- *Objectif :* supprimer `VM_MAX_PROC = 6` ; nombre de process limité par la RAM.
- *Impl :* `Kernel::Process` (déjà déclaré dans `include/Kernel/Process.hpp`) devient
  concret : PID/PPID, état, `Vector<OwnPtr<Thread>>`, table de FD, `cr3`. Les pools de
  pages viennent du futur *frame allocator* (Phase 6.1), plus de tableaux `[6][1024]`.
- *Acceptation :* `ps` liste N process réels avec PID/PPID/état ; N > 6.

**5.3 — `fork()` + `wait()`/`waitpid()` (corrige L6). ⏱️M 🔴**
- *Objectif :* dupliquer un espace d'adressage et attendre la fin d'un enfant.
- *Impl :* `SYS_FORK` clone la `cr3` en **copy-on-write** (dépend de Phase 6.3) ;
  copie la table de FD ; l'enfant reçoit `eax=0`, le parent `eax=pid_enfant`. `SYS_WAIT`
  bloque jusqu'au `Zombie` d'un enfant et récupère son code.
- *Acceptation :* un programme C fait `fork()`, l'enfant écrit un fichier, le parent
  `wait()` puis lit le résultat.

**5.4 — Vraie table de descripteurs de fichiers (corrige L5). ⏱️S 🟡 — ✅ FAIT (2026-07-08)**
- *Objectif :* `open` renvoie un petit entier 0,1,2,3… par processus (pas l'id VFS).
- *Impl (livrée) :* `struct gfd { used, vfs_id, pos }` ; table `fd_tab[FD_MAX_TID][FD_MAX]`
  par thread (= par processus, faute de fork) dans kernel.c. `fd_open_id` alloue le plus
  petit fd libre ≥ 3 (0/1/2 réservés stdin/stdout/stderr) ; `fd_read`/`fd_write` avancent
  un offset par-fd via de nouveaux `vfs_read_at`/`vfs_write_at` ; `fd_lseek` (nouveau
  `SYS_LSEEK`=15, whence SET/CUR/END) repositionne ; `fd_close` libère ; `fd_release_all`
  est appelé par le reaper (Phase 5.1) donc un slot réutilisé n'hérite pas de descripteurs
  périmés. `Syscall.cpp` : open/create/read/write_file/close/lseek passent par la table
  avec `tid = current_id()` (la validation de pointeurs Phase 14.2 est conservée).
- *Acceptation : ✅ vérifiée en QEMU* — `run hello greeting` : `SYS_OPEN → fd=3` (petit
  entier, plus l'id 41), lecture par tranches (read #1 ≠ read #2, l'offset avance),
  `SYS_LSEEK 0` puis relecture == read #1 (rewind) ; `fd=3` de nouveau au lancement suivant
  (table vidée au reap) ; chemin d'écriture intact.
- *Reste (→ plus tard) :* `SYS_STAT`, `SYS_DUP`/`DUP2` (pour les pipes shell, Phase 5.6),
  et câbler 0/1/2 sur un vrai `/dev/tty` (Phase 7.4).

**5.5 — Signaux. ⏱️M 🟡**
- *Objectif :* `kill`, `SIGKILL`, `SIGSEGV`, `SIGCHLD`, handlers utilisateur.
- *Impl :* file de signaux par `Thread` ; livraison au retour de syscall/IRQ (trampoline
  qui pousse un `sigframe` sur la pile user). `try_recover_ring3` (dans `panic.c`) lève
  `SIGSEGV` au lieu de tuer sèchement.
- *Acceptation :* un process installe un handler `SIGSEGV`, déréférence NULL, le handler
  s'exécute et l'appelle proprement.

**5.6 — Pipes & IPC. ⏱️M 🟡**
- *Objectif :* `pipe()`, `|` dans le shell, sockets Unix, mémoire partagée.
- *Impl :* `SYS_PIPE` crée deux FD reliés par un `CircularBuffer` (déjà en `Greg::`) ;
  blocage sur pipe vide/plein → réveille via l'ordonnanceur.
- *Acceptation :* `run producer | run consumer` transfère des octets entre deux process.

**5.7 — Ordonnanceur avancé. ⏱️M 🟡**
- *Objectif :* priorités, `sleep`, blocage sur I/O, équité.
- *Impl :* MLFQ (multi-level feedback queue) ; états `Sleeping`/`Blocked` avec réveil
  par timer ou événement ; `nice`/`SYS_NANOSLEEP`. Supprimer le `hlt` d'attente active.
- *Acceptation :* un process endormi 500 ms ne consomme pas de CPU (mesuré via `top`).

---

### Phase 6 — Mémoire moderne ⏱️L 🔴

*Passer des pools statiques 4 Mo à une VM à la demande, prérequis de `fork` et du SMP.*

**6.1 — Allocateur de frames physiques (corrige L2). ⏱️M 🟡**
- *Impl :* bitmap ou **buddy allocator** couvrant toute la RAM (taille via la carte
  mémoire Multiboot2). `alloc_frame()`/`free_frame()` remplacent les pools statiques
  `proc_*[VM_MAX_PROC][1024]` de `kernel.c`.
- *Acceptation :* allouer/libérer 10 000 frames sans fuite ; `free_frames` exact.

**6.2 — Pagination 4 Ko + *demand paging*. ⏱️M 🔴**
- *Impl :* abandonner PSE 4 Mo pour des tables 4 Ko ; le `#PF` handler (déjà présent
  dans `panic.c`) **mappe la page à la demande** au lieu de faute fatale. Pile user à
  croissance automatique (guard page).
- *Acceptation :* un `mmap` de 64 Mo ne consomme la RAM qu'aux pages réellement touchées.

**6.3 — Copy-on-write (débloque `fork` efficace). ⏱️M 🔴**
- *Impl :* au `fork`, marquer les pages partagées lecture-seule + compteur de refs ;
  au `#PF` en écriture, copier la page. Indispensable pour un `fork` rapide.
- *Acceptation :* `fork` de 100 Mo de heap est instantané ; l'écriture déclenche la copie.

**6.4 — Heap noyau extensible + slab allocator. ⏱️S 🟡**
- *Impl :* `vmalloc` étend le heap noyau au-delà des 32 Mo figés ; caches slab pour les
  objets fréquents (`Thread`, `FileDescriptor`, inodes).
- *Acceptation :* création de 10 000 threads sans OOM prématuré.

**6.5 — `mmap` fichier + mémoire partagée + OOM. ⏱️M 🟡**
- *Impl :* `mmap(fd)` mappe un fichier (page cache partagé, Phase 7.5) ; `MAP_SHARED`
  entre process ; *OOM killer* propre plutôt que triple-faute.
- *Acceptation :* deux process mappent le même fichier et voient les écritures de l'autre.

---

### Phase 7 — Système de fichiers réel ⏱️L 🔴

*Remplacer le VFS plat 64×4 Ko par une vraie couche VFS + un FS on-disk normalisé.*

**7.1 — Couche VFS abstraite (corrige L4). ⏱️M 🟡**
- *Impl :* interfaces `Inode`/`Dentry`/`FileSystem`/`Mount` ; résolution de chemin
  générique ; points de montage. Le VFS mémoire actuel devient un backend `tmpfs`.
- *Acceptation :* monter deux FS différents sous `/` et `/mnt`, naviguer entre eux.

**7.2 — FS on-disk : **ext2** (r/w) ou **GregFS** journalisé. ⏱️L 🔴**
- *Impl :* superblock, groupes de blocs, inodes, bitmaps, indirections ; gros fichiers
  (> 4 Ko), arbre de répertoires réel. Journalisation pour résister aux coupures.
- *Acceptation :* écrire un fichier de 10 Mo, rebooter, le relire intact ; interop
  possible (image ext2 montée sous Linux).

**7.3 — Partitions & montage (corrige partiel L8). ⏱️S 🟡**
- *Impl :* parser **MBR** puis **GPT** ; `mount`/`umount`/`fstab` ; auto-montage au boot.
- *Acceptation :* détecter 2 partitions sur `disk.img`, les monter séparément.

**7.4 — Permissions, métadonnées, FS spéciaux. ⏱️M 🟡**
- *Impl :* `owner/group/mode`, timestamps (mtime/atime/ctime) ; `/dev` (devfs),
  `/proc` (état noyau), `/tmp` (tmpfs). Applique les permissions (dépend Phase 14.1).
- *Acceptation :* `chmod 600` empêche un autre utilisateur de lire ; `cat /proc/meminfo`.

**7.5 — Cache de blocs / page cache. ⏱️M 🟡**
- *Impl :* cache LRU des blocs disque ; écritures différées (writeback) ; `sync`/`fsync`.
  Partagé avec `mmap` fichier (Phase 6.5).
- *Acceptation :* deuxième lecture d'un fichier sert depuis le cache (mesuré).

---

### Phase 8 — Stockage & pilotes ⏱️L 🔴

*Sortir du PIO ; supporter le matériel de stockage moderne (corrige L8).*

- **8.1 — ATA DMA / AHCI (SATA). ⏱️M 🟡** — I/O disque asynchrones par DMA, IRQ de fin.
  *Acceptation :* débit disque ×10 vs PIO, mesuré.
- **8.2 — ISO9660 (CD-ROM) + FAT32. ⏱️M 🟡** — lire le CD de boot ; interop clés USB.
  *Acceptation :* monter et lire une image ISO et une image FAT32.
- **8.3 — Pile USB (xHCI) → HID + Mass Storage. ⏱️L 🔴** — clavier/souris USB, clés USB.
  *Acceptation :* brancher une clé USB (QEMU), la monter, lire/écrire.
- **8.4 — NVMe (optionnel). ⏱️M 🟡** — stockage rapide moderne.
- **8.5 — PCI complet : MSI/MSI-X, énumération, hotplug. ⏱️S 🟡**

---

### Phase 9 — Réseau de production ⏱️L 🔴

*Passer d'un client HTTP en polling à une pile socket complète et chiffrée (corrige L7).*

- **9.1 — RX piloté par IRQ. ⏱️S 🟡 — ✅ FAIT (2026-07-08)**
  *Livré :* ligne IRQ lue en PCI config `0x3C` (IRQ 11 sur QEMU i440fx), gate IDT installée
  au runtime (`kernel_net_irq_install`, stub `irq_net_stub` dans isr.asm), PIC esclave
  démasqué (RMW de `0xA1`). `irq_net_handler` : draine l'anneau matériel → file logicielle
  SPSC (32 slots × 1600 o, indices `volatile`, discipline EventQueue), ack `RTL_ISR`
  (write-1-to-clear, AVANT l'EOI), EOI esclave+maître. `net_poll()` devient le consommateur
  mainline (toute la pile protocolaire reste mono-threadée) + filet anti-IRQ-perdue (drain
  matériel sous `cli` toutes les ~10 jiffies). `IMR=ROK` seul (TX reste spin-wait). Toutes
  les attentes bloquantes (`ping`/`dns`/`dhcp`/`tcp_*`) font `hlt` entre les paquets.
  *Acceptation : ✅ vérifiée en QEMU* — `ping example.com` : RTT stable **20 ms** (« 100 ms
  serait le filet de sécurité seul » → la livraison est bien par interruption), `nslookup`
  et `curl http://example.com` (HTTP 200, 559 o) intacts.
- **9.2 — API socket userland. ⏱️M 🟡** — `SYS_SOCKET/BIND/LISTEN/ACCEPT/CONNECT/SEND/RECV`.
  *Acceptation :* un programme C ouvre une socket et fait une requête HTTP.
- **9.3 — Serveur TCP + robustesse. ⏱️M 🔴** — `listen()`, retransmission, fenêtre,
  contrôle de congestion corrects. *Acceptation :* servir une page à un `curl` de l'hôte.
- **9.4 — TLS 1.2/1.3 → HTTPS. ⏱️L 🔴** — crypto (AES-GCM, SHA-256, X25519/ECDHE, RSA) ;
  `LibCrypto` + `LibTLS`. *Acceptation :* `curl https://example.com` réussit.
- **9.5 — IPv6, cache DNS, pilotes e1000 / virtio-net. ⏱️M 🟡**

---

### Phase 10 — 64 bits + SMP ⏱️L 🔴🔴

*Le grand saut vers un noyau moderne et vraiment parallèle (corrige L1).*

- **10.1 — Port x86-64 (long mode). ⏱️L 🔴🔴** — nouveau boot 64 bits (ou stub 32→64),
  paging 4 niveaux, ABI System V AMD64, réécriture de `switch_task`/`isr.asm`/GDT/TSS.
  *Acceptation :* le même OS boote en 64 bits, RAM > 4 Go adressable.
- **10.2 — ACPI (corrige L11). ⏱️M 🟡 — 🟡 partiel (2026-07-08)** — ✅ RSDP/RSDT/FADT/DSDT
  parsés + **arrêt S5 & reboot ACPI réels** (`drivers/acpi.c`, vérifié en QEMU). Reste :
  parser la MADT pour la découverte des CPU/IRQ (prérequis APIC/SMP, 10.3), tester sur
  vrai matériel.
- **10.3 — APIC / x2APIC + démarrage des APs. ⏱️M 🔴** — Local APIC, I/O APIC, IPI,
  boot des cœurs secondaires. *Acceptation :* `nproc` montre N cœurs actifs.
- **10.4 — Noyau SMP-safe. ⏱️L 🔴🔴** — spinlocks, données par-CPU, TLB shootdown,
  ordonnanceur par-CPU avec *load balancing*. *Acceptation :* stress multi-thread stable
  sur 4 vCPU sans corruption.
- **10.5 — Horloge moderne. ⏱️S 🟡** — HPET, TSC invariant, horloges `MONOTONIC`/`REALTIME`.

---

### Phase 11 — WindowServer userland + Graphismes ⏱️L 🔴

*Sortir la GUI du noyau (corrige L9) et la rendre belle et rapide.*

- **11.1 — WindowServer en Ring 3. ⏱️L 🔴** — le `Compositor` devient un process user
  propriétaire de `/dev/fb0` ; le noyau ne fournit que framebuffer + événements bruts.
  *Acceptation :* tuer le WindowServer ne fait pas paniquer le noyau ; il redémarre.
- **11.2 — IPC LibGUI ↔ WindowServer. ⏱️M 🟡** — mémoire partagée + messages (`Greg::IPC`).
  Chaque fenêtre = un buffer partagé blitté par le serveur.
- **11.3 — Composition optimisée. ⏱️M 🟡** — *dirty rectangles*, *damage tracking*,
  double-buffering matériel, curseur matériel. *Acceptation :* déplacer une fenêtre ne
  redessine que les zones sales (CPU mesuré en baisse).
  - ✅ **Curseur logiciel découplé (fait).** Le curseur est désormais un *overlay* sur le
    front-buffer : `Graphics::present_rect()`/`put_pixel_front()` + `Compositor::move_cursor()`
    effacent l'ancienne empreinte (~10×14 px, restaurée depuis le back-buffer propre) et
    redessinent la flèche à la nouvelle position sur chaque événement souris — au rythme de
    l'IRQ, plus au rythme du recompose plein écran. Le compose plein écran au repos est
    limité (~20 FPS, `COMPOSE_BACKSTOP`) : recompose seulement pour le login, un clic, ou le
    backstop, ce qui libère la boucle pour bouger le curseur entre deux frames. Corrige la
    lenteur perçue du pointeur sur matériel lent. Reste : *dirty rects* pour les fenêtres.
- **11.4 — Rendu de polices TrueType. ⏱️M 🟡** — moteur type FreeType-lite, anti-aliasing,
  hinting. *Acceptation :* afficher du texte lissé à plusieurs tailles.
- **11.5 — GPU & modesetting. ⏱️M 🔴** — virtio-gpu / bochs-dispi, changement de
  résolution à chaud, accélération 2D. *Acceptation :* choisir 1080p au runtime.

---

### Phase 12 — Audio ⏱️M 🟡

- **12.1 — Pilote Intel HDA (ou AC'97/SB16). ⏱️M 🟡** — sortie PCM par DMA.
- **12.2 — Serveur audio + mixeur. ⏱️M 🟡** — mixage multi-app, volume par application.
- **12.3 — Lecteur (WAV puis décodeur MP3/OGG). ⏱️S 🟢** — *Acceptation :* jouer un WAV net.

---

### Phase 13 — Espace utilisateur & toolchain (self-hosting) ⏱️L 🔴🔴

*Le graal SerenityOS : un OS qui se recompile lui-même (corrige L12).*

- **13.1 — LibC (Greg libc) POSIX. ⏱️L 🔴** — `malloc`, `stdio`, `string`, `math`,
  wrappers de syscalls ; en-têtes standard. *Acceptation :* compiler un `.c` POSIX du
  monde réel sans modification.
- **13.2 — Éditeur de liens dynamique (`ld.so`) + `.so`. ⏱️M 🔴** — ELF `ET_DYN`, GOT/PLT,
  relocation au chargement. *Acceptation :* deux programmes partagent une libc `.so`.
- **13.3 — Shell userland + coreutils. ⏱️M 🟡** — le shell devient un process Ring 3 ;
  `ls/cat/cp/mv/grep…` deviennent des ELF séparés dans `/bin`. *Acceptation :* pipeline
  `ls | grep x | wc -l` entre 3 process réels.
- **13.4 — Portage d'un compilateur on-target. ⏱️L 🔴🔴** — d'abord `tcc`, puis `gcc`/`clang`.
  **Jalon final : GregOS se compile lui-même.** *Acceptation :* `cc hello.c && ./hello` sur GregOS.
- **13.5 — Ports / gestionnaire de paquets. ⏱️M 🟡** — recettes de build, dépôt.

---

### Phase 14 — Sécurité ⏱️L 🔴

*Un « meilleur OS » est un OS où le code hostile ne peut pas gagner (corrige L10).*

- **14.1 — Utilisateurs, groupes, authentification réelle. ⏱️M 🟡** — `/etc/passwd`,
  hash de mot de passe (remplace « admin » en dur), UID/GID, `login`, `su`, `sudo`.
  *Acceptation :* deux comptes isolés, permissions appliquées.
- **14.2 — Validation stricte des syscalls. ⏱️S 🔴 — ✅ FAIT (2026-07-08).**
  *Livré :* `vm_validate_user_range(addr,len,need_write)` (kernel.c) parcourt les tables
  de pages du `cr3` courant et n'accepte une plage que si chaque page est Présente + User
  (+ R/W en écriture) — un processus isolé a le noyau en supervisor-only, donc tout
  pointeur noyau est rejeté ; `vm_copy_user_string()` copie sûrement une chaîne user. Tous
  les handlers de `Syscall.cpp` prenant un pointeur user valident désormais : `sys_write`/
  `sys_write_file` (lecture), `sys_read`/`sys_get_heap` (écriture), `sys_open`/`sys_create`
  (copie de nom). `sys_munmap` ne libère plus une adresse arbitraire : un registre des
  allocations kernel-heap rendues par `sys_mmap` (fallback) est tenu, et seule une adresse
  présente dedans est passée à `free()`. *Acceptation : ✅ vérifiée en QEMU* — `run hello
  greeting rapport attack` : les syscalls légitimes marchent, et `SYS_READ`/`SYS_WRITE`/
  `SYS_GET_HEAP` visant `0x00100000` (noyau) renvoient tous `-1` (REFUSE). *Note SMP :* pas
  de TOCTOU en mono-CPU (entrée syscall non ré-entrante) ; à revoir en Phase 10.
- **14.3 — Bacs à sable pledge()/unveil(). ⏱️M 🟡** — un process déclare ses capacités et
  les chemins visibles ; le noyau refuse le reste (modèle SerenityOS).
- **14.4 — Durcissement mémoire. ⏱️M 🔴** — **W^X**, **NX**, **ASLR**/KASLR, canaris de
  pile, **SMEP/SMAP**. *Acceptation :* une page de données n'est jamais exécutable.
- **14.5 — Sécurité du boot. ⏱️S 🟡** — vérification d'intégrité, chaîne de confiance.

---

### Phase 15 — Boot & firmware moderne ⏱️M 🟡

- **15.1 — Boot UEFI (+ GPT). ⏱️M 🟡** — stub UEFI en plus du BIOS/GRUB. *Acceptation :*
  boote sur une machine UEFI-only.
- **15.2 — initramfs / initrd. ⏱️S 🟢** — pilotes/FS de démarrage chargés avant le disque.

---

## Partie III — Chantiers transversaux (permanents)

### III.1 Performance & optimisation
- ✅ **(2026-07-08) Correctif lag GUI** — le thread de debug `test_thread_func` (busy-spin
  Ring-0 hérité de la mise en route de l'ordonnanceur, ~2 M itérations/quantum) n'est plus
  lancé au boot : il volait l'essentiel du CPU à la boucle de rendu du bureau (**28 → 79 FPS**
  mesuré au repos). Le curseur étant redessiné une fois par `compose()`, c'était la cause du
  « curseur qui ne bouge pas ». Reste : composition par *dirty rects* (Phase 11.3) pour les
  machines lentes — le bureau redessine encore tout l'écran à chaque frame.
- Suite de *benchmarks* reproductibles (boot time, débit disque/réseau, latence GUI, ctx-switch/s).
- Profileur échantillonnant (par IRQ timer) + *flame graphs*.
- Zero-copy I/O (page cache partagé, `sendfile`), structures lock-free (déjà `CircularBuffer`).
- Caches par-CPU, réduction du temps de boot (aujourd'hui ~12 s d'animation — rendre asynchrone).
- Composition GUI par *dirty rects* (Phase 11.3) ; TLB shootdown minimal.

### III.2 Fiabilité & observabilité
- `dmesg` : buffer circulaire de logs noyau consultable.
- **Backtraces de panique avec symboles** (table de symboles embarquée) au lieu d'adresses brutes.
- **Stub GDB** sur port série → débogage pas-à-pas du noyau depuis l'hôte.
- Assertions noyau (`ASSERT`/`VERIFY`), `KASAN`-lite (poison des zones libérées).
- Fuzzing des syscalls et des parseurs (ELF, réseau, FS).

### III.3 Tests & CI
- Étendre `run_foundation_tests()` (`kernel/tests.cpp`) à chaque nouveau module.
- **Harnais d'intégration QEMU/QMP existant** (VNC + scancodes AZERTY, screendumps) → transformer
  en suite de non-régression exécutée à chaque commit.
- Matrice de conformité POSIX (quels syscalls, quel comportement).
- Couverture de code, tests de charge (10 000 process/threads).

### III.4 Documentation & i18n
- Manuel (`man`) pour chaque commande ; doc d'architecture par sous-système.
- Internationalisation (l'UI est en français ; prévoir la bascule FR/EN).

---

## Partie IV — Dette technique immédiate (quick wins) ⏱️S

*À traiter en priorité : peu coûteux, fort impact, débloquent la suite.*

1. ~~**Recycler les slots de threads morts (L3)**~~ ✅ **FAIT (Phase 5.1)** — les slots
   de threads, leurs stacks et leur slot VM sont recyclés à l'`exit` ; `run`/`ring3`
   sont désormais illimités par boot.
2. ~~**Vraie table de FD (L5)**~~ ✅ **FAIT (Phase 5.4, 2026-07-08)** — fd = petit entier
   par processus, offset par-fd qui avance sur read/write, `SYS_LSEEK`, table vidée au reap.
3. ~~**`copy_from_user` borné (14.2)**~~ ✅ **FAIT (2026-07-08)** — signalé par une revue
   de sécurité automatisée ; tous les handlers `Syscall.cpp` valident désormais les
   pointeurs user (page-walk Present+User+R/W) et `sys_munmap` ne libère que ses propres
   allocations. Fermait 3 failles CRITIQUES (write/leak/free noyau arbitraires).
4. ~~**Arrêt/reboot ACPI (L11)**~~ ✅ **FAIT (2026-07-08)** — `drivers/acpi.c` : découverte
   RSDP (EBDA + zone BIOS), marche RSDT→FADT (checksums vérifiés), parse AML `_S5` du DSDT
   (SLP_TYPa/b), handshake SMI/SCI_EN, arrêt S5 réel par PM1a/PM1b_CNT (le port `0x604`
   n'est plus câblé : il est *découvert*), reboot par ResetReg ACPI 2.0+ → fallback PS/2.
   Les tables vivant en haut de la RAM (~255 Mo) sont mappées via `paging_map_4mb()`.
   Nouvelle commande `acpi` (rapport de découverte). *Vérifié en QEMU :* `shutdown`
   **éteint réellement la VM** (le process QEMU se termine), `reboot` redémarre jusqu'au
   login, `acpi` affiche RSDP@0xF5290, FACP/APIC/HPET/WAET, PM1a=0x604, SLP_TYP=0.
5. ~~**RX réseau par IRQ (L7)**~~ ✅ **FAIT (Phase 9.1, 2026-07-08)** — voir Phase 9.1.
6. **Relever `MAX_FILES`/taille fichier** — 🟡 *partiel (2026-07-08)* : `FILE_CONTENT_SIZE`
   passé 4 Ko → **8 Ko** (les vrais programmes ELF > 4 Ko tiennent enfin comme fichiers ;
   4 buffers `buf[FILE_CONTENT_SIZE]` de pile passés en `static`). Reste `MAX_FILES=64` et
   l'arborescence — attendent le vrai FS (Phase 7).

---

## Partie V — Définition de « meilleur OS » (critères de fin)

GregOS pourra revendiquer le titre quand **tous** ces critères seront verts :

| Axe | Cible |
|---|---|
| **Isolation** | Aucun process user ne peut lire/écrire la mémoire noyau ni celle d'un autre process ✅(base faite) |
| **Robustesse** | Aucun crash user ne fait tomber le noyau ; le WindowServer redémarre seul |
| **Complétude** | `fork`/`exec`/`wait`/pipes/signals/FD ; FS on-disk ; sockets ; audio ; USB |
| **Performance** | SMP multi-cœurs ; I/O DMA ; GUI par dirty-rects ; boot < 3 s |
| **Sécurité** | Utilisateurs + permissions ; W^X + ASLR ; sandboxing pledge/unveil |
| **Autonomie** | **Self-hosting** : GregOS compile GregOS |
| **Modernité** | 64 bits ; UEFI + ACPI ; TLS/HTTPS ; matériel réel supporté |
| **Qualité** | 0 warning ; CI QEMU verte ; tests + fuzzing ; docs `man` complètes |

---

## Partie VI — Ordre recommandé & dépendances

```
   [Quick wins IV] ──► déblocage immédiat
         │
   Phase 5 (process) ──┬──► Phase 6 (mémoire/COW) ──► fork efficace, SMP
         │             │
   Phase 7 (FS) ◄──────┘         │
         │                        ▼
   Phase 8 (stockage)      Phase 10 (64-bit + SMP) ──► le grand saut
         │                        │
   Phase 9 (réseau/TLS)           ▼
         │                 Phase 11 (WindowServer user)
         ▼                        │
   Phase 13 (libc/toolchain) ◄────┴──► Phase 14 (sécurité) ─► Phase 12 (audio), 15 (UEFI)
                                              │
                                     🏁 Self-hosting = meilleur OS
```

**Prochaine étape conseillée :** attaquer la **Partie IV** (dette immédiate), puis la
**Phase 5** (modèle de processus). Ce sont les fondations sans lesquelles rien d'autre
ne tient — et la Phase 5.1 (recyclage des zombies) est une victoire rapide qui rend
tout le reste testable à grande échelle.

> *« Un noyau n'est jamais fini — il est seulement de plus en plus digne du Drakkar. »*
