<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted Benutzerhandbuch (v0.1.0-alpha)

## Was ist WParted?

WParted ist ein freier Open-Source-Partitionsmanager für Windows. Version 0.1 ist ein **rein lesender
Datenträger-Explorer**: Er zeigt Datenträger, Partitionstabellen, Dateisysteme und Volumes in einer
GParted-ähnlichen Ansicht an und **verändert nichts** auf Ihren Datenträgern.

## Installation und erster Start

1. Laden Sie `WParted-v0.1.0-alpha-win7plus-x64-portable.zip` (bzw. `-x86` für 32-Bit-Windows) von der
   Gitea-Release-Seite herunter und prüfen Sie die SHA-256-Prüfsumme aus `…-checksums.sha256`.
2. Entpacken Sie das ZIP in einen beliebigen Ordner. Keine Installation und keine Laufzeitumgebung nötig.
3. Starten Sie `wparted.exe`.

Ohne Administratorrechte zeigt WParted nur die von Windows eingebundenen Volumes; die Bereiche
dazwischen erscheinen **schraffiert als „unbekannt“**. Um die echten Partitionstabellen zu lesen, klicken
Sie im gelben Hinweisbalken auf **Als Administrator neu starten** (oder *WParted → Als Administrator neu
starten*).

## Das Hauptfenster

| Bereich | Zweck |
|---|---|
| Werkzeugleiste | Aktualisieren sowie die aus GParted bekannten Partitionsaktionen. Noch nicht verfügbare Aktionen sind ausgegraut; der Tooltip erklärt den Grund. |
| Geräteauswahl | Datenträger wählen. `[Windows]` kennzeichnet den Datenträger mit dem laufenden Windows. |
| Datenträgergrafik | Jedes Feld ist eine Partition in proportionaler Größe. Randfarbe = Dateisystem, gelbe Füllung = belegter Platz, grau = nicht zugeordnet, schraffiert = unbekannt. ←/→ wechselt, Eingabe zeigt Details. |
| Partitionsliste | Partition, Name, Dateisystem, Einhängepunkt, Bezeichnung, Größe, belegt, frei, Flags und Status. Doppelklick/Eingabe zeigt Details, Rechtsklick öffnet das Partitionsmenü. |
| Ausstehende Operationen | Zeigt in späteren Versionen geplante Änderungen an. |
| Statuszeile | Suchergebnis, gefundene Tabellenwarnungen und ob WParted mit Administratorrechten läuft. |

## Nützliche Funktionen

- **Ansicht → Geräteinformationen**: Modell, Seriennummer, Schnittstelle, Sektorgrößen, SSD,
  Schreibschutz, Tabellentyp, GPT-Zustand und alle Diagnosen.
- **Partition → Informationen**: Sektorbereich, Ausrichtung (1 MiB und physischer Sektor), GUIDs,
  Attribute, Flags, Einhängepunkte und Belegung.
- **Ansicht → Dateisystemunterstützung**: was dieser Build je Dateisystem kann.
- **Gerät → Bericht exportieren**: speichert einen JSON-Bericht aller Datenträger (hilfreich für Fehlerberichte).
- **Ansicht → Design**: System, Hell oder Dunkel.
- **Werkzeuge → Protokollordner öffnen**: `%LOCALAPPDATA%\WParted\logs`.

## Kommandozeile

```text
wparted-cli list [--json]                        Datenträger, Partitionen, Volumes
wparted-cli inspect-image disk.img [--sector-size 4096] [--json]
wparted-cli fs-support [--json]
wparted-cli version
```

Für Partitionstabellen aus einer Administrator-Eingabeaufforderung starten.

## Häufige Fragen

**Ist WParted kostenlos?** Ja, GPL-2.0-or-later.
**Kann WParted Partitionen verkleinern oder löschen?** Noch nicht. Diese Operationen sind geplant und werden
erst freigeschaltet, wenn sie Tests auf virtuellen Datenträgern bestanden haben.
**Unterstützt WParted Windows 7?** Windows 7 SP1 ist Build-Ziel, wurde aber noch nicht auf einem echten
Windows-7-System geprüft.
**Kann WParted Linux-Partitionen lesen?** Es erkennt ext2/3/4, btrfs, XFS, Swap, LUKS, LVM u. a. und zeigt
Bezeichnungen, wo möglich. Änderungen erfordern den geplanten Offline-Modus.
**Gehört WParted zu GParted?** Nein. WParted folgt dem GParted-Bedienkonzept, ist aber ein unabhängiges Projekt.
**Wo melde ich Fehler?** https://git.tracht-digital.de/jtracht/WParted/issues – möglichst mit exportiertem Bericht.
