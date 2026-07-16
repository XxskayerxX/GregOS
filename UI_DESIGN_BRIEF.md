# BRIEF DE DESIGN — GregOS : « Le Terminal du Royaume »
### Refonte visuelle complète : CRT phosphore rétro‑futur × lore Greg & Drakkar, avec un familier (Greg + dragon) animé et réactif aux actions de l'OS.

> **Copie-colle ce document entier dans une nouvelle conversation avec ton assistant de design IA.** Il est auto‑suffisant : tout le contexte (lore, contraintes du moteur de rendu, API graphique, inventaire des apps) est inclus. Si l'assistant a accès au dépôt GregOS, il pourra en plus lire les vrais fichiers (chemins donnés §3.4).

---

## 0. Ton rôle et le niveau exigé

Tu es **directeur artistique d'un studio réputé pour donner à chaque produit une identité qu'on ne peut confondre avec aucune autre.** Le client a déjà rejeté tout ce qui sentait le template. Tu es payé pour un **point de vue tranché** : des choix délibérés et justifiés de palette, de « typographie », de chrome et de mouvement, spécifiques à CE projet — et **une vraie prise de risque assumée** (ici : le familier animé, voir §5).

Contraintes de goût, non négociables :
- **Zéro défaut d'IA.** Évite les trois clichés actuels : (1) fond crème + serif contrasté + terracotta ; (2) fond quasi‑noir + un seul accent acid‑green/vermillon ; (3) mise en page « journal » à filets fins. Ici la direction est déjà posée (§2) : tout doit lire comme **le phosphore d'un tube cathodique appartenant à un royaume‑noyau norrois.**
- **Dépense ton audace à UN seul endroit** (le familier Greg & Drakkar). Le reste — chrome, grille, icônes — reste discipliné et cohérent.
- **Cohérence radicale des couleurs :** tout passe par des *rampes de phosphore* (ambre, vert, braise), pas par un arc‑en‑ciel. L'actif vs inactif se joue en **luminosité** (brillant vs éteint), pas en teinte.
- **Chaque décision se justifie par le sujet** (le lore ou le tube CRT), jamais « parce que c'est joli ».

---

## 1. Le sujet : qu'est‑ce que GregOS

GregOS est un **véritable OS x86 32 bits bare‑metal** (bootloader GRUB2, framebuffer VBE, C/C++ freestanding, sans libc) — avec un bureau, un gestionnaire de fenêtres, un shell, et ~18 applications. Ce n'est pas une maquette web : **ce que tu dessines doit être rendu pixel par pixel par le moteur logiciel décrit au §3.** L'interface est en **français**.

### Le lore (matière première du design — exploite‑le partout)
GregOS est raconté comme un **royaume médiéval‑norrois** régné par un roi‑noyau et son dragon :
- **GREG 1er, Seigneur du Kernel** — « invoqué depuis le néant du premier boot ».
- **Drakkar**, le **dragon** — « Ensemble, ils règnent sur chaque cycle d'horloge de GregOS depuis l'aube. »
- Un **chevalier** et un **dragon** gardent déjà l'écran de login ; le boot montre « Greg & Drakkar » et un logo dragon.
- Économie du royaume : **GregCoins** (monnaie du casino). Le navigateur = **GregNet** (URLs `greg://`). Le shell = **gregsh**. Le FS = **gregfs**. La version = « GregOS v2.0 ».
- Ton in‑world, chaleureux et épique mais jamais lourd : l'OS parle comme le royaume de Greg (décrets, présages, sceaux), tout en restant **utilisable**.

### La vibe recherchée (mots‑clés)
Tube cathodique ambré qui chauffe · phosphore qui rémanence · scanlines · une **machine ancienne et puissante** · runes qui luisent · fer forgé et or · souffle de dragon vert‑phosphore · un roi qui veille sur les cycles d'horloge.

---

## 2. La vision esthétique : fusion **CRT phosphore** × **fantasy norroise**

Deux mondes fondus en un seul langage :

- **Couche CRT / rétro‑futur :** l'écran est un **tube au phosphore**. Fond obsidienne quasi‑noir légèrement verdâtre, texte et UI qui **luisent** (halo/bloom léger), **scanlines**, **vignette** (coins plus sombres, illusion de bombé), léger **flicker**/persistance, aberration chromatique ponctuelle. Rendu quasi‑monochrome par zones : une zone « parle » en **ambre**, une autre en **vert**, les alertes en **braise**.
- **Couche royaume norrois :** le **chrome** des fenêtres est un **cartouche héraldique** (bordures en traits doubles CP437, or‑rune, sceaux) ; les surfaces documents peuvent virer **parchemin** (manuscrit enluminé affiché sur le CRT) ; les icônes sont des **blasons pixel‑art phosphore** ; le curseur est une **petite flamme** ou une pointe d'épée.
- **La fusion se voit surtout dans le familier (§5)** : Greg (le roi) et Drakkar (le dragon), dessinés **au phosphore**, vivent dans l'interface et **réagissent** à ce que fait l'utilisateur.

Métaphore directrice : **« Un tube cathodique enchanté par lequel Greg, Seigneur du Kernel, et son dragon Drakkar veillent sur la machine. »**

---

## 3. CONTRAINTES TECHNIQUES DU MOTEUR — non négociables

Tout ce que tu proposes **doit être implémentable avec ce moteur.** Pas de CSS, pas de blur GPU, pas de PNG à alpha.

### 3.1 Surface & couleur
- Résolution fixe **800 × 600**, **32 bits par pixel**, format **0x00RRGGBB** (XRGB, l'octet haut est ignoré). **Couleur 24 bits pleine** disponible (pas de palette imposée) — mais par choix de design on se **restreint** aux rampes de phosphore.
- **Double buffer logiciel** : on dessine dans un back‑buffer puis `swap_buffers()` recopie tout à l'écran. Le compositeur **redessine tout l'écran à chaque frame** (pas encore de dirty‑rect) → **budget perf réel** : privilégie les effets **précalculés/bakés** et les zones **locales** pour l'animation.
- **PAS d'alpha‑blending / pas de transparence par pixel.** La seule transparence est une **couleur‑clé** (`0xFFFFFFFF` = pixel non dessiné lors d'un blit). Donc : **pas de vrai verre, pas de flou, pas d'ombre semi‑transparente.** Les ombres se font en **bandes sombres pleines décalées** ; le « glow » se **fake** (voir §4.3).

### 3.2 Police (« typographie »)
- **Une seule police bitmap : Terminus 8×16**, **monospace**, **256 glyphes CP437** (donc tu as les **traits de filet** `─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼`, doubles `═ ║ ╔ ╗ ╚ ╝ ╠ ╣`, blocs `█ ▓ ▒ ░ ▀ ▄ ▌ ▐`, flèches, etc.).
- **Pas d'anti‑aliasing, pas de crénage variable :** chaque glyphe occupe **exactement 8 px de large × 16 px de haut**. Le texte est **grille fixe**.
- Tu peux dessiner un caractère avec **couleur de premier plan + couleur de fond** (ou fond transparent via couleur‑clé). Tu **n'as pas** d'autres tailles nativement — mais tu peux **doubler/tripler les pixels** au blit pour un titre 2× (16×32) ou un logo 3–4×.
- **Stretch autorisé** (optionnel, plus de boulot) : proposer une **police d'affichage bitmap custom** (ex. capitales + chiffres + runes, ~40 glyphes, dessinée à la main comme tableau C) pour les titres/logo. Spécifie‑la si tu la proposes.

### 3.3 API graphique disponible (c'est TOUTE ta boîte à outils)
Le renderer est la classe `Graphics` (aussi exposée en C via `gfx_*`). Primitives :

```
put_pixel(x, y, color)                              // 1 pixel
fill_rect(x, y, w, h, color)                         // rectangle plein
draw_hline(x, y, len, color) / draw_vline(...)       // lignes 1px
draw_rect(x, y, w, h, color)                         // contour 1px
draw_char(x, y, c, fg, bg)                           // 1 glyphe 8x16 (bg = GFX_TRANSPARENT pour fond transparent)
draw_str(x, y, "texte", fg, bg)                      // chaîne
gradient_rect(x, y, w, h, c1, c2, vertical)          // dégradé LINÉAIRE 2 couleurs (vertical ou horizontal)
draw_image(x, y, w, h, pixels[])                     // blit d'un tableau XRGB top-down ; pixel == 0xFFFFFFFF => transparent (color-key)
gfx_draw_bmp_memory(data, x, y)                      // blit d'un BMP 24 bits non compressé embarqué
set_clip(x,y,w,h) / clear_clip()                     // rectangle de découpe
swap_buffers()                                       // présenter la frame
present_rect(x,y,w,h) / put_pixel_front(x,y,color)   // écriture directe front-buffer (déjà utilisé par le curseur logiciel)
```
Constantes : `GFX_FONT_W = 8`, `GFX_FONT_H = 16`, `GFX_RGB(r,g,b)`, `GFX_TRANSPARENT = 0xFFFFFFFF`.
**Assets image :** on peut **embarquer** des sprites/fonds soit en **BMP 24 bits** (comme le fond de login `gregbg.bmp`, blitté par `gfx_draw_bmp_memory`), soit en **tableau C `unsigned int[]` XRGB** (avec color‑key `0xFFFFFFFF`) blitté par `draw_image`. Un générateur type `tools/make_elf_header.py` transforme un binaire en header C.
**Timing/animation :** compteur global **`jiffies` à 100 Hz** (100 ticks/seconde). Toute animation se cadence sur `jiffies` (ex. « frame = (jiffies / 10) % 8 » = 10 fps).

### 3.4 Carte de l'architecture (fichiers réels à cibler — si accès au repo)
- `include/Theme.hpp` — **LE système de design vit ici** : des `constexpr unsigned int` (tokens couleur). C'est le premier fichier à réécrire.
- `include/Graphics.hpp` + `drivers/gfx.cpp` — le renderer (ajouter ici les helpers CRT : glow, scanlines…).
- `kernel/Compositor.cpp` — le bureau : `draw_wallpaper()`, `draw_desktop()`, `draw_desk_icons()`, `draw_taskbar()`, `draw_cursor_front()`, `compose()`. **C'est ici que vivront le fond CRT, la taskbar et le familier.**
- `include/DesktopIcons.hpp` — table des 9 icônes du bureau (`{label, x, y}`, cases 48×48).
- `kernel/LoginWindow.cpp` — art de boot/login (chevalier + dragon actuels).
- Chaque app = `kernel/<Nom>Window.cpp` avec une méthode `draw(Graphics& g)` et `on_event()`. Les fenêtres sont des `Window` (chrome commun : barre de titre, bordure, boutons `[_][▢][✕]`).
- `kernel/kernel.c` — boucle principale ; pilote `wm_draw()` (compose) à un rythme limité ; expose les hooks OS (voir §5.3).

---

## 4. Le système de design à livrer (tokens)

### 4.1 Palette — point de départ (à affiner, pas à copier bêtement)
Rampes de phosphore sur obsidienne, plus les accents du royaume. Donne les valeurs finales sous forme de tokens `Theme::` (hex).

| Rôle | Nom token | Hex (départ) | Usage |
|---|---|---|---|
| Fond tube (éteint) | `CRT_OBSIDIAN` | `#0B0E0C` | bureau, fond client, « verre » du tube |
| Phosphore ambre (primaire) | `PHOS_AMBER` | `#FFB000` | titres/décrets, texte royal, accent chaud |
| Phosphore ambre sombre | `PHOS_AMBER_DIM` | `#7A5410` | halo/scanline de l'ambre, texte secondaire |
| Phosphore vert (système) | `PHOS_GREEN` | `#33FF66` | terminal, succès, texte « machine » |
| Phosphore vert sombre | `PHOS_GREEN_DIM` | `#124D24` | halo/inactif vert |
| Braise de dragon (feu/alerte) | `EMBER` | `#E8933A` → `#FF4A1C` | souffle du dragon, alertes, suppression |
| Or‑rune (royal, bordures/sceaux) | `RUNE_GOLD` | `#C9A24B` (clair `#F0C674`) | cartouches actifs, sceaux, blasons |
| Teal arcane (liens/secondaire) | `ARCANE_TEAL` | `#2AA198` | liens GregNet, accents froids (clin d'œil au teal historique) |
| Parchemin (rare, documents) | `VELLUM` | `#E8DCC0` (encre `#2A2416`) | surfaces « manuscrit » (éditeur/lecteur) — usage parcimonieux |
| Cendre (texte muet, filets) | `ASH` | `#4A5548` | libellés discrets, filets de grille |
| Sang (panique/fatal) | `BLOOD` | `#8B1A1A` | écran de panique, banqueroute |

Règle : **une fenêtre/zone choisit UNE rampe dominante** (ex. Terminal = vert‑phosphore, décrets système = ambre, casino = or/braise). L'unité vient de là.

### 4.2 « Typographie » bitmap — comment faire vivre le 8×16
Le monospace n'est pas une limite, c'est **l'identité** (« le terminal du royaume »). Hiérarchie sans changer de taille :
- **Couleur** (rampe de phosphore) = niveau 1 de hiérarchie.
- **CAPITALES** pour titres/décrets ; casse normale pour le corps.
- **Espacement** : insère des espaces pour « tracker » un titre (`G R E G O S`).
- **Vidéo inverse** (fg/bg permutés) pour sélection, en‑têtes de colonne, barre de titre active.
- **Filets CP437** : doubles `═ ║ ╔╗╚╝` pour les cadres **royaux/actifs**, simples `─│┌┐` pour panneaux internes, blocs `▓▒░` pour barres de progression et « souffle ».
- **Échelle par multiples entiers** (pixel‑doubling) : corps 8×16 · titres 2× (16×32) · logo héro 3–4×.
- **Caret** clignotant (cadencé `jiffies`) = petite **flamme** plutôt qu'un bloc.
- (Stretch) police d'affichage runique custom pour le mot « GregOS » et les titres.

### 4.3 Effets CRT — **recettes implémentables sans alpha**
Donne pour chacun une recette concrète (fonction à ajouter, coût perf) :
- **Scanlines :** après compose, assombrir 1 ligne sur 2 (ou 1/3) de ~12–18 % — soit un `fill`/`hline` sombres cadencés, soit un multiply précalculé. Prévoir un **toggle** (perf/confort).
- **Glow / bloom (fake) :** dessiner glyphe/forme **deux fois** — d'abord un « halo » en couleur *dim* décalé de ±1 px, puis le **cœur brillant**. Fournir `draw_str_glow()`.
- **Vignette + bombé du tube :** **baker** un assombrissement radial dans le fond (coins plus sombres) ; option cadre/bezel 1–2 px autour de l'écran.
- **Flicker / persistance :** jitter **très subtil** (≤ 5 %) de luminosité globale, lent, cadencé `jiffies`. **Respecter un mode « mouvement réduit »** (toggle) — pas de stroboscopie.
- **Aberration chromatique :** décaler le canal R (et/ou B) de 1 px **uniquement** sur titres/arêtes à fort contraste, avec parcimonie.
- **Power‑on / power‑off :** allumage = une ligne horizontale qui s'ouvre en pleine trame + warm‑up flicker ; extinction = collapse en ligne puis point, fade phosphore.

### 4.4 Iconographie (bureau, 48×48)
Chaque icône = **blason pixel‑art phosphore** (rampe 2 couleurs + halo), style cohérent, libellé en ambre discret dessous. Redessine les **9** existantes (renomme in‑world si pertinent) :
`Terminal (gregsh)`, `Fichiers (gregfs)`, `GregNet (navigateur)`, `Système`, `Casino (GregCoins)`, `Jeux (arcade)`, `Calc`, `Paint`, `Horloge`.

### 4.5 Curseur
Petite **flamme phosphore** ou **pointe d'épée** (le curseur est déjà un overlay logiciel ~10×14 px, blitté sur le front‑buffer). Garde‑le lisible sur fond sombre **et** clair (contour).

---

## 5. ⭐ LA SIGNATURE : Greg & Drakkar, le familier réactif

**C'est LE truc qu'on retiendra de GregOS, et la demande explicite du client.** Greg (le roi‑noyau) et **Drakkar** (son dragon) vivent dans l'interface, dessinés au phosphore, et **réagissent en temps réel à ce que fait l'OS.** Pense « Clippy », mais un **roi norrois et son dragon**, épique et rendu CRT — et **jamais agaçant** (discret au repos, expressif aux moments clés).

### 5.1 Concept & placement
- **Drakkar** est le compagnon **permanent** : perché dans un **bezel de commandement** (bandeau bas / coin bas‑droit de la taskbar), toujours visible, il réagit en continu.
- **Greg** apparaît pour les **grands moments** (boot, login, victoire, panique, extinction) — plein cadre ou en médaillon.
- Le familier doit pouvoir s'animer **sans forcer un recompose plein écran** : réserve‑lui une **zone locale** redessinée seule (comme le curseur), cadencée `jiffies`.

### 5.2 Spécs sprites (à détailler)
- Drakkar : ~**96×64 px** (propose), **4–8 frames par état**, tableau C XRGB + color‑key, rampe phosphore (corps vert‑phosphore, œil/braise ambre, feu = rampe braise).
- Greg : ~**64×96 px** pour les moments héroïques.
- Cadence mascotte **~8–12 fps** (`frame = (jiffies / N) % nb_frames`). Boucles d'idle courtes, transitions d'événement prioritaires.
- Fournir une **feuille de sprites décrite** (états + nb de frames) et le budget mémoire/perf.

### 5.3 ⭐ TABLE DE RÉACTIONS (événement OS → animation) — le cœur du livrable
Pour **chaque** ligne : storyboard court (3–6 mots par frame clé) + **le hook OS réel** où brancher `familiar_react(EVENT)`. Complète/raffine :

| Événement | Hook OS (où brancher) | Animation Greg / Drakkar |
|---|---|---|
| Allumage / GRUB→kernel | début `kernel_main` | Le tube **s'allume** (warm‑up, flicker) ; Greg endormi sur le trône, Drakkar enroulé qui **ouvre un œil**. |
| Séquence de boot / tests | `run_foundation_tests` | Drakkar **souffle une flamme** phosphore qui « **forge** » la barre de progression. |
| Écran de login | `LoginWindow` | Greg se **dresse**, main sur l'épée‑noyau ; curseur = flamme. |
| Login réussi (`admin`) | `g_login_done` | Drakkar **déploie ses ailes**, un **feu vert‑phosphore balaie** le bureau (transition d'entrée). |
| Login échoué | mauvais mdp | Drakkar **renâcle** (fumée), Greg **secoue la tête**, bref **glitch** rouge. |
| **Idle** (aucune entrée) | `jiffies` depuis dernière entrée | Drakkar **somnole** : clignements, battement de queue, **volutes de fumée** toutes ~5 s ; flicker ambiant. |
| Commande shell lancée | `execute_command` | Drakkar **dresse la tête**, **yeux qui s'allument**. |
| Commande réussie | retour OK | petit **hochement**, étincelle verte. |
| Erreur / fichier introuvable | chemin d'erreur | Drakkar **gronde**, **braise rouge**, texte d'alerte. |
| Fichier **sauvegardé** | `vfs_write_file` / Ctrl+S | Drakkar **appose un sceau de cire doré** (petit « stamp »). |
| Fichier **supprimé** | `vfs_delete` | Drakkar **incinère** l'icône (flammèche), **cendres** qui tombent. |
| Fenêtre ouverte/fermée | ouverture/`close` | Drakkar **suit du regard** ; ouverture = déploiement en scanline. |
| **Casino : gain** | solde ↑ / victoire | Drakkar **crache une pluie de GregCoins** dorées, yeux en pièces. |
| **Casino : perte / banqueroute** | solde = 0 | Drakkar **se dégonfle**, fumée grise, Greg **soupire**. |
| **Réseau up** (DHCP/GregNet) | `net_up` | Drakkar **prend son envol**, porte un **corbeau‑paquet**. |
| Téléchargement / requête HTTP | activité réseau | Drakkar **bat des ailes**, va‑et‑vient. |
| Musique / bip | speaker on | Drakkar **dodeline** en rythme. |
| **Panique / fault noyau** | panic handler | Drakkar **rugit plein écran**, **rouge sang**, scanlines violentes, Greg brandit l'épée — **écran de panique héroïque** (in‑world). |
| **Extinction** (shutdown) | `acpi_shutdown` | Greg **rengaine l'épée**, Drakkar **se rendort**, le CRT **s'éteint** (collapse + fade). |
| Reboot | `acpi_reboot` | Drakkar **avale l'écran** et le **recrache** (transition). |

### 5.4 API du familier (à spécifier, implémentable)
- Un **état** `familiar_state` (enum : IDLE, ALERT, FIRE, SEAL, COINS, FLY, ROAR, SLEEP…) + `familiar_frame`.
- `familiar_react(EVENT)` : appelé depuis les hooks ci‑dessus, fixe l'état + une priorité + une durée (en `jiffies`).
- `familiar_tick()` / rendu dans `compose()` : avance la frame sur `jiffies`, blitte la frame courante dans la zone réservée, revient à IDLE après la durée.
- **Mode « mouvement réduit »** : version statique/discrète (le familier reste mais n'anime que sur événements majeurs).

---

## 6. Bureau · taskbar · boot · login

- **Bureau** = champ **obsidienne CRT** (vignette bakée + faible **grille de runes** ou champ d'étoiles animé très subtil), filigrane d'un **blason royal**. Remplace le teal historique. Icônes = §4.4.
- **Taskbar** = **« Bandeau de Commandement »** en bas : bouton Démarrer = sigil rune **`◈ GREG`** (ouvre le menu = « **Grimoire** »), boutons de fenêtres = onglets phosphore (actif brillant / inactif éteint), horloge = **afficheur à digits luisants**, et **Drakkar perché** à droite (§5).
- **Boot** = allumage CRT + réveil de Greg & Drakkar + décret « **GREG 1er, Seigneur du Kernel, s'éveille du néant du premier boot** » + barre de progression « forgée » par le feu du dragon.
- **Login** = panneau **salle du trône**, champ mot de passe façon **sceau**, succès = **essuyage au feu de dragon** vers le bureau. (Réutilise/upgrade le chevalier + dragon déjà présents.)

---

## 7. Refonte app par app (18 fenêtres) — priorisée

Traite d'abord **P0**, puis P1, puis P2. Pour chacune : rampe dominante, chrome, traitement des états (vide, chargement, erreur, sélection).

- **P0 — fondations d'identité :**
  - `TerminalWindow` (**gregsh**) : la vitrine. Vert‑phosphore, glow, scanlines, caret‑flamme, en‑tête « décret ».
  - **Bureau + taskbar + curseur** (§6, §4.5).
  - **Boot + LoginWindow** (§6).
  - **Le familier Greg & Drakkar** (§5).
- **P1 — apps phares :**
  - `CasinoWindow` + `PokerWindow` : or/braise, GregCoins qui brillent, feutre → « table du royaume », réactions du dragon.
  - `FileManagerWindow` (**gregfs**) : coffres/parchemins, sceaux, toolbar créer/supprimer/renommer.
  - `BrowserWindow` (**GregNet**) : chrome `greg://`, liens teal arcane, état de chargement « corbeau‑messager ».
  - `TextEditorWindow` / `TextViewerWindow` : surfaces **parchemin** (manuscrit enluminé sur CRT), numéros de ligne en gouttière.
- **P2 — le reste (cohérence) :**
  - `CalcWindow`, `ClockWindow` (Horloge — cadran runique/afficheur nixie‑phosphore), `SystemMonitorWindow` (jauges/graph en phosphore), `SystemWindow` (« À propos du Royaume »), `ImageViewerWindow`, `GamesWindow` (cartouches d'arcade), `StartMenuWindow` (le **Grimoire**), `ContextMenuWindow` (menu clic‑droit = **parchemin déroulé**), `ArcadeApp` (host plein écran).

---

## 8. Mouvement & retenue
Inventaire des animations avec **durées en `jiffies`** et un **fallback mouvement‑réduit** pour chacune : power‑on/off du tube, ouverture/fermeture de fenêtre (déploiement scanline), caret‑flamme, idle du dragon, réactions événementielles (§5.3), transitions de login/shutdown. **Moins = plus** partout **sauf** sur le familier : c'est lui qui porte l'audace. Évite l'animation gratuite (signe d'IA).

---

## 9. Copy / voix in‑world (français)
L'OS parle comme le **royaume de Greg** — **une** touche in‑world par surface, le reste **clair et utilisable** (un libellé étiquette, une erreur explique quoi faire).
- Dossier vide : « **Ce coffre est vide. Forge un premier parchemin.** »
- Erreur : « **Le Kernel refuse : \<raison\>.** » (jamais d'excuse molle, jamais vague).
- Décrets système en CAPITALES ambre ; messages machine en vert.
- Fournis une **table de chaînes** (état → texte FR) pour boot, login, erreurs, vides, casino, shutdown.

---

## 10. Livrables attendus (format de ta réponse)
Rends un document **markdown auto‑suffisant** contenant :
1. **Système de tokens** → valeurs concrètes pour `include/Theme.hpp` (tous les `constexpr`, anciens + nouveaux noms).
2. **Spéc des effets CRT** : recettes implémentables (fonctions à ajouter à `gfx.cpp`/`Compositor.cpp`, coût perf, toggles).
3. **Chrome de fenêtre** : mockup **ASCII** + dimensions px + couleurs (actif/inactif, boutons, bordures CP437).
4. **Bureau + taskbar** : mockup ASCII + specs.
5. **Jeu d'icônes 48×48** : description pixel‑art de chacune des 9.
6. ⭐ **LE FAMILIER** : placement, feuille de sprites (dimensions + frames/état), **la table de réactions complète** (événement → animation → hook OS), l'**API** (`familiar_react` / `familiar_tick` / états), budget perf.
7. **Notes de refonte app par app** (les 18), priorisées P0→P2.
8. **Storyboard boot + login** (frames clés).
9. **Inventaire mouvement** (durées en `jiffies` + fallback mouvement‑réduit).
10. **Guide de voix + table de chaînes FR**.
11. **Plan d'implémentation** : quels fichiers toucher, dans quel ordre, mappé à l'archi réelle (§3.4) ; comment embarquer les sprites (tableau C XRGB avec color‑key **ou** BMP 24 bits).

Formate avec des **mockups ASCII**, des **tokens hex**, et des **tableaux**. Sois concret : chaque écran doit pouvoir être codé tel quel avec l'API du §3.3.

---

## 11. Garde‑fous qualité (relis‑toi avant de rendre)
- Chaque surface lit comme **« phosphore CRT d'un royaume‑noyau norrois »** — pas comme un template.
- **Audace concentrée** sur Greg & Drakkar ; chrome discipliné, rampes de phosphore cohérentes (pas d'arc‑en‑ciel).
- **Moteur respecté** : aucun alpha/flou ; uniquement les primitives du §3.3 ; conscient de la perf (recompose plein écran coûteux → effets bakés + zones locales).
- **Une prise de risque justifiée** : le familier réactif comme **citoyen de première classe** de l'OS.
- **UI française et utilisable** : les fioritures in‑world ne doivent jamais nuire à la clarté.
- Avant de rendre : enlève **un** ornement de trop (règle Chanel).

---

*Fin du brief. Réponds avec le document markdown complet décrit au §10.*
