<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted: Häufige Fragen

Antworten zu WParted 0.8.0-beta.1, einem freien Partitionsmanager für Windows 11. In English: [FAQ](FAQ.md).

## Allgemein

### Was ist WParted?
WParted ist ein freier Open-Source-Partitionsmanager für Windows 11 mit GParted-ähnlicher Bedienung. Er zeigt Datenträger
und Partitionen, plant Änderungen und führt sie auf Image-Dateien und angehängten virtuellen Datenträgern aus.

### Ist WParted kostenlos?
Ja. WParted ist kostenlos und steht unter der GNU GPL v2.0 oder neuer.

### Ist WParted eine GParted-Alternative für Windows?
In der Bedienung ja, im Umfang noch nicht. WParted übernimmt Datenträgerkarte, Warteschlange und Anwenden von GParted und
läuft direkt unter Windows, das Schreiben auf echte Datenträger ist aber noch gesperrt. Es ist ein eigenständiges Projekt.

### Kann ich GParted ohne Live-USB unter Windows nutzen?
GParted selbst nicht, denn es braucht Linux. WParted läuft direkt unter Windows 11 und arbeitet ähnlich, sodass Sie
Datenträger prüfen und Änderungen planen können, ohne etwas anderes zu starten.

### Welche Windows-Versionen unterstützt WParted?
Nur Windows 11. Windows 7, 8 und 10 werden weder unterstützt noch getestet.

### Sendet WParted Daten?
Nein. WParted hat keine Telemetrie, keine automatische Update-Prüfung und nutzt kein Netzwerk.

### Braucht WParted eine Installation?
Nein. WParted ist ein portables ZIP: entpacken und `wparted.exe` starten.

## Sicherheit

### Kann WParted meine echten Datenträger beschädigen?
Diese Version schreibt nicht auf echte Datenträger, denn dieser Weg ist bis zu einem menschlichen Sicherheitsreview
gesperrt. Sie schreibt nur auf Image-Dateien und angehängte virtuelle Datenträger, die Sie wählen und bestätigen.

### Kann WParted den Datenträger des laufenden Windows ändern?
Nein. WParted schreibt nie auf einen Datenträger mit dem laufenden Windows, dessen Auslagerungsdatei oder Bootmanager.

### Funktioniert WParted mit BitLocker?
WParted erkennt BitLocker und verändert, pausiert oder entschlüsselt es nie. Pläne, die ein geschütztes Volume ändern würden, werden abgelehnt.

### Was passiert bei einem Stromausfall während einer Operation?
Eine unterbrochene Verschiebung lässt sich fortsetzen, weil WParted ein Journal führt. Schritte, die Daten direkt ändern,
lassen sich nicht fortsetzen: WParted meldet sie, und Sie stellen das Backup wieder her. Nach dem Anwenden gibt es kein allgemeines Rückgängig.

## Aufgaben

### Wie teste ich Partitionsänderungen unter Windows gefahrlos?
Mit einer VHDX. Legen Sie in Windows eine an, hängen Sie sie an, starten Sie WParted als Administrator, wählen Sie den
virtuellen Datenträger, planen Sie die Änderungen und wählen Sie *Anwenden*. Ihre echten Datenträger bleiben unberührt.

### Kann WParted NTFS-Partitionen verkleinern und vergrößern?
Ja, auf Image-Dateien und angehängten virtuellen Datenträgern. Beim Verkleinern verschiebt es Daten aus dem wegfallenden
Bereich. Auch eine `$MFT` mit Attributliste wird verschoben; eine beschädigte bricht das Verkleinern vor jeder Änderung ab.

### Kann WParted eine Partition verschieben?
Ja, auf Image-Dateien und angehängten virtuellen Datenträgern. Die Daten werden blockweise kopiert, und eine unterbrochene Verschiebung lässt sich fortsetzen.

### Kann WParted ext4 und andere Linux-Partitionen unter Windows lesen?
Es erkennt ext2/3/4, btrfs, XFS, Swap, LUKS und LVM und zeigt ihre Bezeichnungen. ext2/3/4 kann es auf Image-Dateien und
virtuellen Datenträgern anlegen, in der Größe ändern und prüfen, aber Sie können die Dateien darauf nicht öffnen.

### Kann WParted eine gelöschte Partition wiederherstellen?
Es kann freien Platz nach verlorenen Dateisystemen durchsuchen und das Neuanlegen planen. Der Scan echter Datenträger ist
rein lesend und braucht Administratorrechte; das Neuanlegen läuft auf Image-Dateien und virtuellen Datenträgern.

### Kann WParted ein Dateisystem reparieren?
Nein. WParted prüft nur lesend und überlässt Reparaturen `chkdsk` und `e2fsck`, damit keine Reparatur Daten zerstört, die diese Werkzeuge noch retten könnten.

### Hat WParted eine Kommandozeile?
Ja. `wparted-cli` listet Datenträger mit `--json`, untersucht Images, prüft Pläne im Probelauf und wendet sie mit `--execute` an.
Siehe [Benutzerhandbuch](user/de/benutzerhandbuch.md).

## Projekt

### Ist WParted fertig?
Nein, es ist eine Beta. Der [Projektstatus](PROJECT-STATUS.md) nennt, was fertig ist, was noch von Hand geprüft werden muss und was offen ist.

### Wo melde ich Fehler?
Über [GitHub Issues](https://github.com/jtracht/WParted/issues). Sicherheitsprobleme bitte nach [SECURITY.md](../SECURITY.md).
