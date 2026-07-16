# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Qu'est-ce que GregOS

OS x86 **32 bits from scratch** (bare-metal, freestanding — pas de libc, pas de libgcc),
bootable via GRUB2/Multiboot2, avec bureau graphique 800×600×32, pile TCP/IP réelle,
**HTTPS avec vraie vérification de certificats**, Ring 3 isolé, et une identité visuelle
**CRT phosphore × lore norrois « Greg & Drakkar »** (UI en français, ton in-world).
Mot de passe de login : **`admin`**. La vision long-terme est dans **`ROADMAP.md`**,
le brief design complet dans **`UI_DESIGN_BRIEF.md`**, les specs/plans d'implémentation
dans **`docs/superpowers/`**.

## Règles de collaboration (NON NÉGOCIABLES)

1. **Ne JAMAIS exécuter `git add` / `git commit` / `git push`** — fournir les commandes
   à l'utilisateur, c'est lui qui les lance. Git en lecture (status/log/diff) est OK.
   Ne jamais inclure de trailer `Co-Authored-By` dans les messages de commit proposés.
2. **Mettre à jour `README.md` à CHAQUE changement de code** (c'est la vitrine GitHub du
   projet — la maintenir belle et exacte).
3. **Discipline de vérification** avant de déclarer un incrément terminé :
   build **0 warning** ; `nm <obj> | grep -iE ' U (__mul|__udiv|__div|__mod)'` vide
   (pas de helper libgcc) ; tests hôte quand le module s'y prête ; test **bout-en-bout
   en QEMU piloté par QMP** (voir Harnais) ; pour tout morceau substantiel, une **revue
   adversariale** (les revues ont systématiquement trouvé de vrais bugs que les tests
   nominaux rataient : bypass Marlinspike, nameConstraints/EKU, entropie faible,
   racine minimax budgetée…).

## Build & exécution

```bash
make            # noyau + ISO GRUB (myos.iso). 0 warning attendu.
make run        # QEMU avec -cpu max (OBLIGATOIRE : expose RDRAND pour le CSPRNG TLS),
                # RTL8139 (réseau slirp), audio PC speaker, disque ATA disk.img
make run-kvm    # variante KVM
make run-debug  # -no-reboot -no-shutdown, log des resets CPU dans /tmp/qemu-gregos.log
                # + capture des printf 0xE9 dans /tmp/gregos-debug.log (-debugcon)
make clean      # ⚠️ SUPPRIME disk.img → le recréer :
                #   dd if=/dev/zero of=disk.img bs=512 count=2048
```

- Le Makefile **globbe** `kernel/*.c`, `kernel/*.cpp` (+ Greg/, GUI/, drivers/) et les
  headers d'`include/` (racine, Greg/, Kernel/ .h+.hpp, GUI/, Compositor/) : **créer un
  fichier suffit** (aucun edit Makefile), mais **modifier n'importe quel header
  reconstruit TOUT** (`%.o: %.cpp $(HEADERS)`).
- Seul `drivers/keyboard.c` (legacy) est **exclu** du build — remplacé par
  `kernel/PS2Keyboard.cpp`.
- Flags : C `-m32 -ffreestanding -O2 -Wall -Wextra` ; C++ idem + `-fno-exceptions
  -fno-rtti -fno-threadsafe-statics -std=c++11`.
- `kernel.c` fournit `memcpy`/`memset` (GCC peut en émettre à -O2 : ça linke).

## Tests

```bash
# Suites hôte (le MÊME code C que le noyau, compilé nativement) :
cd tests/host
cc -std=c11 -O2 -Wall -Wextra -I../../include -o /tmp/hnt hn_test.c ../../kernel/hnefatafl_core.c && /tmp/hnt
cc -std=c11 -O2 -Wall -Wextra -o /tmp/dgt dungeon_gen_test.c && /tmp/dgt
```

- **`kernel/hnefatafl_core.c` est LE patron à réutiliser** pour toute logique riche :
  module **C pur sans dépendance noyau**, ramassé par le glob du Makefile côté noyau
  ET compilé tel quel sur l'hôte par la suite de tests → zéro dérive entre le code
  testé et le code embarqué. (`dungeon_gen_test.c` est un *port* de la génération du
  Donjon — si on touche `DungeonWindow::gen_level`, mettre le port à jour.)
- La crypto/TLS a été validée par **KAT contre Python `cryptography`** (vecteurs
  générés hôte, exécutés sur le vrai code kernel compilé hôte) + PKIs d'attaque
  OpenSSL (chaînes Marlinspike, pathLen, nameConstraints/EKU) + captures de vrais
  handshakes rejoués. Reconstruire ces fixtures au besoin (elles étaient dans un
  scratchpad éphémère) ; la méthode est documentée dans le README.

## Harnais QEMU/QMP (test bout-en-bout piloté)

`tools/qmp.py` : driver QMP minimal (clavier qcode, souris relative, screenshots).

```python
# Lancer QEMU headless EN TÂCHE DE FOND (sinon le sandbox tue le process).
# Ne pas oublier -cpu max ici aussi (sinon pas de RDRAND → TLS sur entropie faible) :
qemu-system-i386 -cdrom myos.iso -vga std -m 256M -cpu max -display none \
  -netdev user,id=n0 -device rtl8139,netdev=n0 \
  -drive file=disk.img,format=raw,if=ide,index=0 \
  -debugcon file:/tmp/gregos-debug.log \
  -qmp unix:/tmp/gregos-qmp.sock,server,nowait
# Puis piloter depuis un process séparé : QMP().key(...), .move_to(x,y), .click(), .shot(png)
```

Pièges du harnais, tous vécus :
- **Clavier AZERTY** (scancodes set 1, table FR dans `PS2Keyboard.cpp`) : les qcodes QMP
  sont des positions physiques QWERTY → pour taper `admin` au login, envoyer
  **`q d semicolon i n`**. Attendre ~14 s (boot + animation Drakkar) avant de taper.
- **Souris** : déplacements par pas ≤ 5 px (accélération ×2 du pilote à ≥ 6 px/paquet) ;
  `home_mouse()` pour recaler en (0,0).
- Une fenêtre fraîchement ouverte via le menu n'a **pas le focus clavier** : cliquer sa
  barre de titre d'abord.
- `screendump` écrit un `.ppm` → convertir avec ImageMagick ; comparer des pixels entre
  2 frames (PIL) pour prouver une animation.
- `pkill` sans correspondance sort en code 144 (cosmétique) ; `-debugcon file:` garde son
  offset → une troncature externe laisse des trous NUL (strip `b"\0"`).

## Architecture (vue d'ensemble)

- **`arch/i386/`** — boot.asm (Multiboot2, VBE), isr.asm, switch_task.asm, linker.ld.
- **`kernel/kernel.c`** (~12 500 lignes) — cœur historique : boot, GDT/IDT/IRQ, pilotes
  bas niveau, VFS mémoire (+ persistance ATA « GRFS » sur `disk.img`), **shell 110+
  commandes**, ordonnanceur préemptif 100 Hz (`jiffies`), Ring 3 (INT 0x80, ELF32),
  `memcpy`/`memset`, boucle d'événements GUI. **Ajouter une commande shell = DEUX
  edits** : la branche dans `execute_command()` ET l'entrée dans `cmd_list[]` (qui
  alimente complétion Tab / help / type / which — l'oublier ne casse rien au build
  mais rend la commande invisible).
- **`drivers/`** — `gfx.cpp` (framebuffer **double buffer logiciel**, primitives),
  `window_manager.cpp` (dispatch événements : taskbar → menus → clavier → fenêtres →
  icônes bureau ; **resize min 160×80**), `event.cpp`, ACPI, ATA, ports. Le clavier et
  la souris sont dans **`kernel/`** : `kernel/PS2Keyboard.cpp` (AZERTY) et
  `kernel/PS2Mouse.cpp` (parser à **auto-resynchronisation** — ne pas le « simplifier »,
  il corrige un bug historique de désync de trames) ; headers dans `include/Kernel/`.
- **`kernel/Compositor.cpp`** — bureau : wallpaper **baké en cache** (`s_wp_cache`,
  silhouette de Drakkar incluse), Anneau des Cycles **animé par frame** dans
  `draw_desktop()`, scanlines CRT (`apply_scanlines` avant curseur), taskbar, icônes
  (table partagée `include/DesktopIcons.hpp`, 10 icônes — utilisée par le rendu ET le
  hit-test, ne jamais éditer l'un sans l'autre). **Règle perf : statique → baké dans
  `draw_wallpaper` ; animé → `draw_desktop`, léger.**
- **`kernel/LoginWindow.cpp`** — boot theatre : phase LOADING (DRAKKAR en ASCII-art
  plein écran, `include/art_drakkar.h`, braises `lw_draw_embers`), phase LOGIN (GREG 1ᵉʳ,
  `art_greg.h`). Les gardiens sont animés **par palette** (`lw_tri`/`lw_mix`, entiers
  seulement) — **la grille de glyphes ne bouge JAMAIS** (compo validée par l'utilisateur).
- **Fenêtres** (`kernel/*Window.cpp`, 20) — sous-classes de `Window`
  (`draw()`/`on_event()`/`handle_char()`, géométrie `client_*`). Apps : Browser (GregNet),
  Terminal, FileManager, TextEditor/Viewer, ImageViewer (BMP), Paint, Calc, Clock,
  SystemMonitor, System, Casino (5 jeux), Poker, Games (launcher arcade plein écran via
  `launch_arcade_game_async`), Minesweeper, **Dungeon** (roguelike), **Hnefatafl**
  (tablut 9×9 vs IA minimax), StartMenu, ContextMenu, Login.
- **Réseau** (`kernel/net.c`) — RTL8139 (PCI, IRQ), ARP/IPv4/ICMP/UDP/**TCP client**/
  DNS/DHCP/**HTTP/1.1** (requête 1.1 + `Connection: close`, décodage *chunked* —
  d'anciens commentaires du fichier disent « 1.0 », c'est le fil qui fait foi).
  Shell : `ping`, `nslookup`, `curl`, `wget`…
- **TLS/PKI** (`kernel/tls.c`, `crypto.c`, `p256.c`, `p384.c`, `rsa.c`, `x509.c`,
  `certverify.c`, `include/cert_roots.h`) — TLS 1.2 ECDHE (x25519 + P-256),
  AES-128-GCM, **vérification de chaîne complète** : signatures jusqu'à un magasin de
  **144 racines Mozilla embarquées**, hostname (SAN/wildcards/CN), dates (RTC CMOS),
  basicConstraints/keyUsage/EKU/pathLen, **fail-closed sur extension critique
  inconnue**, signature ServerKeyExchange liée au leaf (anti-MITM actif), CSPRNG
  RDRAND (d'où `-cpu max`). Erreurs typées → page « Certificat invalide » du navigateur.
- **`Greg/`** — mini-bibliothèque (String, Vector, HashMap, RefPtr…), `GUI/` — widgets.
- **`programs/`** — ELF Ring-3 embarqués (blackjack, userapp) via `tools/make_elf_header.py`.

## Contrat moteur graphique (à respecter dans TOUTE UI)

- **Pas d'alpha-blending.** Transparence = color-key `GFX_TRANSPARENT` (0xFFFFFFFF) uniquement.
- Primitives : `Graphics::instance()` → `fill_rect`, `draw_rect`, `draw_hline/vline`,
  `draw_char/str(x,y,c,fg,bg)` (fonte bitmap **8×16 CP437**, `drivers/font8x16.h`),
  `gradient_rect` (2 couleurs), `put_pixel`, `draw_image` (XRGB), `present_rect`.
- Couleurs **uniquement via les tokens `include/Theme.hpp`** (rampes : AMBER=roi,
  GREEN=machine, GOLD=métal, EMBER=feu, BLOOD=sang, + VELLUM parchemin). ⚠️ Piège
  récurrent du re-skin : un fond re-mappé sombre casse tout texte encore codé
  `0x000000` — **vérifier le texte, pas seulement les fonds**.
- **Fonte CP437 sans décodage UTF-8** : tout texte AFFICHÉ (`draw_str`, titres de
  fenêtres, shell) doit être en français **sans accents** (« Demineur », « Systeme »,
  « Etage ») — un « é » UTF-8 fait deux octets → deux mauvais glyphes à l'écran.
  Accents OK uniquement dans les commentaires et les .md.
- Timing : `extern "C" volatile unsigned long jiffies` (100 Hz). Pas de flottants —
  animations par ondes triangulaires/mix entiers (voir `lw_tri`/`lw_mix`).
- RNG : xorshift entier seedé sur `jiffies` (déterministe, pas de `rand()`).

## Checklist « ajouter une app fenêtrée »

1. `include/FooWindow.hpp` + `kernel/FooWindow.cpp` (classe + factory
   `extern "C" void open_foo_window(void)` : `Greg::make_ref<FooWindow>()`,
   `win->setup(x,y,w,h,"Titre",bg)`, `WindowManager::instance().add_window(...)`).
   Aucun edit Makefile (glob).
2. `kernel/StartMenuWindow.cpp` : extern + libellé + `case` — **⚠️ insérer une entrée
   DÉCALE tous les `case` suivants du switch `launch_item` : les remapper** ; bump
   `N_ITEMS` dans le `.hpp`.
3. `kernel/ContextMenuWindow.cpp` : idem + une entrée dans `s_icon_color[]`.
4. (Optionnel, icône bureau) `include/DesktopIcons.hpp` + art 48×48 dans
   `Compositor::draw_desk_icons` + `case` dans `drivers/window_manager.cpp`.
5. **Patron obligatoire pour les fenêtres à layout fixe** (le WM autorise le resize
   jusqu'à 160×80) : (a) gater le bloc souris d'`on_event` par
   `hit_test(e.mouse.x, e.mouse.y)` ; (b) garde en tête de `draw()` : si
   `client_w()/client_h()` < layout → afficher « Trop petit » et return.
   (Exemples complets : Hnefatafl, Minesweeper ; le Donjon — clavier pur, pas de bloc
   souris — n'illustre que (b).)
6. README + vérifs (règles 2 et 3 ci-dessus).

## Leçons durement acquises (ne pas ré-apprendre)

- **Les revues adversariales trouvent ce que les tests nominaux ratent.** Exemples réels :
  trust-anchoring contournable (Marlinspike), extensions critiques ignorées (fail-open),
  entropie prédictible, éval statique optimiste qui détrône un coup minimax complet à
  l'épuisement du budget. Toujours en lancer une sur du code sécurité/moteur.
- **Budget de nœuds IA** : `HN_NODE_BUDGET=25000` (~5 ms hôte ≈ 0,1-0,3 s sous QEMU TCG).
  À l'épuisement, **`break` à la racine** (garder le meilleur coup complètement calculé).
- L'ordre des branches d'erreur compte : dans le navigateur, `code < 0` (erreurs typées)
  doit être testé **avant** `!body` — un bug réel d'affichage venait de là.
- `read_record` TLS : rejeter `len <= 0` (un record vide bouclait à l'infini).
- Le lore EST la direction produit : l'utilisateur veut du **grand, sombre, menaçant**
  (ASCII-art plein écran) — pas de mascottes mignonnes. « Improve » sur un visuel validé
  = animer la **palette**, jamais la géométrie.

## État actuel & prochaine étape

- **Triptyque du Royaume** (3 apps, spec `docs/superpowers/specs/2026-07-15-triptyque-royaume-design.md`) :
  ✅ **TRIPTYQUE COMPLET (2026-07-16)** : 1/3 Donjon de Drakkar · 2/3 Hnefatafl ·
  3/3 Chant Runique (`kernel/RuneChantWindow.cpp` + données C pur
  `kernel/runechant_data.c` testées `tests/host/runechant_test.c`). Le son marche :
  ponts `timer_speaker_on/off` (timer_c.h), mélodies en thread `scheduler_spawn`,
  annulation par compteur de génération + cli/sti, `speaker_off` garanti `on_removed`.
  **Patron de vérif audio à réutiliser** : QEMU `-audiodev wav,id=snd0,path=x.wav
  -machine pc,pcspk-audiodev=snd0` enregistre le haut-parleur (⚠️ le WAV omet les
  silences — le gate fermé n'enregistre pas) → analyse zero-crossing sur l'hôte.
- Durcissements TLS optionnels restants (non exploitables réseau) : RSA-SHA384/512,
  efficacité du magasin de racines, OCSP/CRL, TLS 1.3/ChaCha20.
- UI possibles : police Px437, glow par glyphe, clipping systémique dans
  `Graphics::fill_rect` (couvrirait le resize de toutes les fenêtres d'un coup).
- Voir `ROADMAP.md` pour la vision long-terme (self-hosting, 64 bits, WindowServer Ring 3…).
