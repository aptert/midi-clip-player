# MIDI Clip Player — Plugin LV2 pour Mod Dwarf

Un plugin LV2 qui lit des fichiers MIDI depuis la Mod Dwarf et permet de choisir le clip à jouer via un dropdown. Le changement de clip se fait à la fin du clip en cours (seamless loop switching).

## Ce dont tu as besoin

- Un Mac (ou Linux)
- [Docker Desktop](https://www.docker.com/products/docker-desktop/) installé et lancé
- [VSCode](https://code.visualstudio.com/) (optionnel)
- Une Mod Dwarf branchée en USB
- Des fichiers MIDI uploadés sur la Dwarf dans **User Files > MIDI Songs** via le file manager (`http://moddwarf.local`)

---

## Installation en 4 étapes

### Étape 1 — Cloner le projet

```bash
git clone <url-du-repo>
cd midi-clip-player
```

### Étape 2 — Compiler le plugin

Lance Docker Desktop, puis dans le terminal :

```bash
docker run --rm \
  -v $(pwd):/workspace \
  cbix/mod-plugin-builder:moddwarf \
  bash -c "
    /root/mod-workdir/moddwarf/toolchain/bin/aarch64-mod-linux-gnu-gcc \
      -shared -fPIC -fvisibility=hidden \
      -std=c99 -static-libgcc \
      -I/root/mod-workdir/moddwarf/host/usr/aarch64-buildroot-linux-gnu/sysroot/usr/include \
      --sysroot=/root/mod-workdir/moddwarf/host/usr/aarch64-buildroot-linux-gnu/sysroot \
      -o /workspace/midi_clip_player.so \
      /workspace/src/plugin.c -lm 2>&1
  "
```

Ça prend quelques minutes la première fois (Docker télécharge ~1.3GB). Si ça se termine sans message d'erreur, c'est bon.

### Étape 3 — Déployer sur la Mod Dwarf

Branche ta Dwarf en USB, puis :

```bash
# Remonter le filesystem en écriture
ssh root@moddwarf.local "mount -o remount,rw /"
# Mot de passe : mod

# Créer les dossiers
ssh root@moddwarf.local "mkdir -p /root/.lv2/midi_clip_player.lv2/modgui && mkdir -p /usr/lib/lv2/midi_clip_player.lv2/modgui"

# Copier les fichiers du bundle
rsync -av --rsh="ssh" \
  midi_clip_player.lv2/ \
  root@moddwarf.local:/root/.lv2/midi_clip_player.lv2/

rsync -av --rsh="ssh" \
  midi_clip_player.lv2/ \
  root@moddwarf.local:/usr/lib/lv2/midi_clip_player.lv2/

# Copier le .so compilé
rsync -av --rsh="ssh" \
  midi_clip_player.so \
  root@moddwarf.local:/root/.lv2/midi_clip_player.lv2/midi_clip_player.so

rsync -av --rsh="ssh" \
  midi_clip_player.so \
  root@moddwarf.local:/usr/lib/lv2/midi_clip_player.lv2/midi_clip_player.so

# Redémarrer l'interface
ssh root@moddwarf.local "systemctl restart mod-ui"
```

> Le mot de passe SSH de la Mod Dwarf est `mod`.

### Étape 4 — Utiliser le plugin

1. Ouvre `http://moddwarf.local` dans ton navigateur
2. Ajoute le plugin **MIDI Clip Player** au pedalboard
3. Connecte sa sortie MIDI vers un synthé (ex: MDA JX10)
4. Connecte la sortie audio du synthé vers la sortie audio
5. Le plugin liste automatiquement tous tes fichiers `.mid` depuis `User Files > MIDI Songs`
6. Choisis ton clip dans le dropdown — il repart du début à chaque changement

---

## Ajouter tes fichiers MIDI

Va sur `http://moddwarf.local`, clique sur **User Files**, puis **MIDI Songs** et upload tes fichiers `.mid`.

Ensuite mets à jour les `scalePoints` dans `midi_clip_player.lv2/plugin.ttl` avec les noms de tes fichiers :

```turtle
lv2:scalePoint [ rdfs:label "mon_clip_1" ; rdf:value 0 ] ,
               [ rdfs:label "mon_clip_2" ; rdf:value 1 ] ,
               [ rdfs:label "mon_clip_3" ; rdf:value 2 ]
```

Et ajuste `lv2:maximum` avec le nombre de clips - 1.

Puis redéploie avec les commandes rsync de l'étape 3.

---

## Contrôle via MIDI (Keystep MK2 ou autre)

Le plugin répond à deux types de messages MIDI entrants :

- **Program Change** (0-15) → sélectionne le clip correspondant
- **Note On** sur les notes C2 à D#3 (MIDI 36-51) → sélectionne les clips 0-15

---

## Dépannage

**L'interface ne répond plus après un déploiement :**
```bash
ssh root@moddwarf.local "systemctl restart jack2 && sleep 5 && systemctl restart mod-ui"
```

**Le filesystem est en read-only :**
```bash
ssh root@moddwarf.local "mount -o remount,rw /"
```

**Le plugin n'apparaît pas dans la liste :**
```bash
ssh root@moddwarf.local "lv2ls | grep midi-clip"
```

---

## Structure du projet

```
midi-clip-player/
├── src/
│   └── plugin.c              ← code source C du plugin
├── midi_clip_player.lv2/
│   ├── manifest.ttl          ← index LV2
│   ├── plugin.ttl            ← description des ports
│   ├── modgui.ttl            ← interface web (optionnel)
│   └── modgui/
│       └── icon.html         ← template HTML de l'interface
└── README.md
```
