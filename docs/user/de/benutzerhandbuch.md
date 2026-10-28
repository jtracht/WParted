<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted Benutzerhandbuch (v0.1.0-alpha; Planung und Rettung: Entwicklungs-Builds von v0.2)

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
| Detailbereich | Eigenschaften der gewählten Partition bzw. des freien Bereichs und warum eine Aktion nicht verfügbar ist. |
| Ausstehende Operationen | Die geplanten Änderungen in Reihenfolge mit Risiko und Prüfstatus (Entwicklungs-Builds). |
| Statuszeile | Suchergebnis, gefundene Tabellenwarnungen und ob WParted mit Administratorrechten läuft. |

## Operationen planen (Entwicklungs-Builds von v0.2)

Entwicklungs-Builds können Änderungen wie in GParted **planen**. Es wird nichts geschrieben: Diese
Version kann Operationen nicht ausführen, *Anwenden* zeigt nur die Prüfungen.

1. Partition oder nicht zugeordneten Bereich in der Datenträgergrafik oder Liste wählen.
2. Aktion wählen: *Neu*, *Löschen*, *Größe ändern/Verschieben*, *Kopieren*/*Einfügen*, *Formatieren als*,
   *Partition benennen*, *Dateisystembezeichnung*, *Neue UUID*, *Flags verwalten*, *Prüfen* oder
   *Gerät → Partitionstabelle erstellen*. Die Dialoge zeigen eine Vorschau des Ergebnisses; Größen in MiB,
   Ausrichtung standardmäßig 1 MiB (*Ausrichten an: Keine* verzichtet auf das Aufrunden auf eine MiB-Grenze).
3. Die Datenträgergrafik zeigt das **künftige Layout**; *Ausstehende Operationen* listet jeden Schritt.
   *Rückgängig* (Strg+Z) entfernt den letzten, *Bearbeiten → Alle Operationen verwerfen* alle.
4. *Anwenden* (Strg+Eingabe) prüft den Plan und zeigt eine Zusammenfassung mit Risiken und blockierenden
   Fehlern (z. B. Systempartitionen, belegte oder verschlüsselte Volumes, Partitionen kleiner als ihre
   Daten, Überlappungen, MBR-Grenzen). Dort lässt sich der **Plan exportieren** (JSON) oder **Details
   speichern (HTML)**.
5. *Bearbeiten → Plan speichern* (Strg+S) speichert den Plan. Er enthält die Identität des Datenträgers
   (Seriennummer, Größe, Tabellen-GUID/-Signatur), damit ein Plan nicht unbemerkt auf einen anderen
   Datenträger angewendet werden kann.

Zum gefahrlosen Ausprobieren *WParted → Datenträger-Image öffnen* (Strg+O) mit einer Rohabbild-Datei
(`.img`) verwenden.

## Plan auf eine Image-Datei anwenden

Bei einer geöffneten Image-Datei prüft *Anwenden* den Plan für das Image und bietet, wenn er besteht,
**Auf Image-Datei anwenden…** an. Eine zweite Bestätigung nennt die Datei; eine automatische
Rückgängig-Funktion gibt es nicht, legen Sie daher bei Bedarf vorher eine Kopie an. WParted

- schreibt jede Operation und **liest die Partitionstabelle zurück**, bevor die nächste beginnt (die
  erste Abweichung stoppt den Plan),
- verschiebt und kopiert Partitionen blockweise (jedes Dateisystem, gleiche Größe),
- löscht alte Dateisystem-Signaturen am Anfang neuer Partitionen,
- zeigt jeden Schritt im Ergebnis und liest das Image neu ein.

Auf Images möglich:

| | erstellen | vergrößern | verkleinern | verschieben / kopieren | prüfen | Bezeichnung | neue UUID |
|---|---|---|---|---|---|---|---|
| FAT16 / FAT32 | ✓ | ✓ | ✓ (Daten werden umgelagert) | ✓ | ✓ nur lesend | ✓ | ✓ |
| NTFS | – | ✓ | ✓ wenn der Bereich hinter dem neuen Ende frei ist | ✓ | – | ✓ | ✓ |
| exFAT | ✓ | – | – | ✓ | – | ✓ | ✓ |
| ext2 / ext3 / ext4 | ✓ (Entwicklungs-Builds) | ✓ innerhalb der Deskriptorblöcke (Entwicklungs-Builds) | ✓ wenn der Bereich hinter dem neuen Ende frei ist (Entwicklungs-Builds) | ✓ | ✓ nur lesend (Entwicklungs-Builds) | ✓ | ✓ |
| Linux-Swap | – | – | – | ✓ | – | ✓ | ✓ |
| andere | – | – | – | verschieben ✓, kopieren wo sicher | – | – | – |

Außerdem: neue Partitionstabellen, neue oder gelöschte Partitionen, Flags und Namen. Verschieben und
Kopieren arbeiten blockweise bei gleicher Größe; eine Kopie in eine größere FAT-Partition wird
vergrößert. Pläne mit Schritten, die WParted nicht kann, werden abgelehnt, bevor etwas geschrieben
wird – ebenso das Verkleinern von NTFS mit Daten hinter dem neuen Ende und Dateisysteme mit Fehlern.
Physische Datenträger beschreibt diese Version nie.

Entwicklungs-Builds nach 0.2 verschieben auch erweiterte Partitionen (ihre Grenzen; die logischen
Partitionen bleiben, wo sie sind).

### Unterbrochene Ausführungen (Entwicklungs-Builds nach 0.2)

Während WParted einen Plan anwendet, führt es ein Journal neben dem Image (`<Image>.wparted-journal`).
Fallen Programm, Rechner oder Strom aus oder brechen Sie während eines Verschiebens ab, zeigt das
Journal, wo die Ausführung stehen geblieben ist:

- **zwischen zwei Operationen** oder **beim blockweisen Verschieben oder Kopieren**: WParted kann genau
  dort weitermachen. Die GUI bietet beim nächsten Öffnen des Images *Fortsetzen* an; auf der
  Kommandozeile zeigt `wparted-cli resume <Image>` den Stand, `--execute` setzt fort. Bis dahin liegen
  die Daten einer unterbrochenen Verschiebung teils an der alten, teils an der neuen Position –
  verwenden Sie das Image nicht.
- **während eines Schritts, der Daten an Ort und Stelle ändert** (Dateisystem vergrößern, verkleinern
  oder erstellen, Tabelle schreiben): das lässt sich nicht fortsetzen; WParted sagt das, und das Image
  kann inkonsistent sein. Stellen Sie es aus Ihrer Kopie wieder her.

*Journal verwerfen* (bzw. `--discard`) löscht nur das Journal, nie das Image. Einzelheiten:
[Fehlerfälle](../../architecture/failure-modes.md) (englisch).

## Datenrettung (nur lesend)

*Gerät → Datenrettung versuchen* durchsucht den nicht zugeordneten Bereich des gewählten Datenträgers oder
Abbilds nach Dateisystemen, die keine Partition mehr abdeckt (z. B. nach dem Löschen einer Partition).
Geprüft werden 1-MiB-Schritte und alte Zylindergrenzen; die Suche läuft im Hintergrund und liest nur.

Das Ergebnis nennt Startsektor, Dateisystem, Größe, Bezeichnung und ob der Beginn MiB-ausgerichtet ist.
Die Wiederherstellung ist **nicht automatisiert**: Sie können in diesem Bereich eine *neue* Partition
planen und prüfen; sie wird aber in ganzen MiB platziert und passt nur zu einem Dateisystem mit
MiB-ausgerichtetem Beginn. Für physische Datenträger sind Administratorrechte nötig.

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
wparted-cli rescue-scan-image disk.img [--sector-size 4096] [--json]
wparted-cli apply-plan disk.img plan.json [--execute] [--log details.html]   prüfen bzw. auf das Image anwenden
wparted-cli version
```

Für Partitionstabellen aus einer Administrator-Eingabeaufforderung starten.

## Häufige Fragen

**Ist WParted kostenlos?** Ja, GPL-2.0-or-later.
**Kann WParted Partitionen verkleinern oder löschen?** Noch nicht. Entwicklungs-Builds können diese
Operationen planen und prüfen, aber nicht ausführen. Die Ausführung wird erst freigeschaltet, wenn sie
Tests auf virtuellen Datenträgern bestanden hat.
**Unterstützt WParted Windows 7?** Windows 7 SP1 ist Build-Ziel, wurde aber noch nicht auf einem echten
Windows-7-System geprüft.
**Kann WParted Linux-Partitionen lesen?** Es erkennt ext2/3/4, btrfs, XFS, Swap, LUKS, LVM u. a. und zeigt
Bezeichnungen, wo möglich. Änderungen erfordern den geplanten Offline-Modus.
**Gehört WParted zu GParted?** Nein. WParted folgt dem GParted-Bedienkonzept, ist aber ein unabhängiges Projekt.
**Wo melde ich Fehler?** https://git.tracht-digital.de/jtracht/WParted/issues – möglichst mit exportiertem Bericht.
