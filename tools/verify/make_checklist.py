#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates docs/verification/WParted-Pruefcheckliste.pdf (needs reportlab)."""
from pathlib import Path
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import KeepTogether, ListFlowable, ListItem, Paragraph, Preformatted, SimpleDocTemplate, Spacer, Table, TableStyle

OUT = Path(__file__).resolve().parents[2] / "docs" / "verification" / "WParted-Pruefcheckliste.pdf"
BASE = getSampleStyleSheet()
BODY = ParagraphStyle("body", parent=BASE["BodyText"], fontName="Helvetica", fontSize=9.5, leading=12.5)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=9, leading=11.5)
H1 = ParagraphStyle("h1", parent=BASE["Title"], fontName="Helvetica-Bold", fontSize=19, alignment=0, spaceAfter=4)
H2 = ParagraphStyle("h2", parent=BASE["Heading2"], fontName="Helvetica-Bold", fontSize=12.5, spaceBefore=11, spaceAfter=4, keepWithNext=1, textColor=colors.HexColor("#1F3A5F"))
CODE = ParagraphStyle("code", fontName="Courier", fontSize=8, leading=10, backColor=colors.HexColor("#F1F3F5"), borderPadding=4, spaceBefore=4, spaceAfter=8)
WARN = ParagraphStyle("warn", parent=BODY, backColor=colors.HexColor("#FDECEA"), borderColor=colors.HexColor("#C0392B"), borderWidth=0.8, borderPadding=6, spaceBefore=6, spaceAfter=10)


def p(text, style=BODY):
    return Paragraph(text, style)


def code(text):
    return Preformatted(text, CODE)


def checks(rows):
    """rows: (what to do, expected result)"""
    data = [[p("<b>OK</b>", CELL), p("<b>Nr.</b>", CELL), p("<b>Schritt</b>", CELL), p("<b>Erwartetes Ergebnis</b>", CELL)]]
    for step, expected in rows:
        COUNTER[0] += 1
        data.append(["", p(str(COUNTER[0]), CELL), p(step, CELL), p(expected, CELL)])
    t = Table(data, colWidths=[9 * mm, 9 * mm, 76 * mm, 76 * mm], repeatRows=1)
    style = [
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#AAB2BD")),
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#E8EDF3")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("TOPPADDING", (0, 0), (-1, -1), 3),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
    ]
    t.setStyle(TableStyle(style))
    return t


COUNTER = [0]


def footer(canvas, doc):
    canvas.saveState()
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(colors.grey)
    canvas.drawString(20 * mm, 10 * mm, "WParted - Pruefcheckliste, Stand 2026-11-01")
    canvas.drawRightString(190 * mm, 10 * mm, f"Seite {doc.page}")
    canvas.restoreState()


def build():
    s = [p("WParted: Prüfcheckliste", H1)]
    s.append(p("Prüft alles, was der Agent nicht selbst testen durfte oder konnte. Hake jeden Schritt ab; notiere bei Abweichungen die Nummer und die Meldung. "
               "Dauer: etwa 45 Minuten. Du brauchst Windows 11 x64 und Administratorrechte."))
    s.append(p("<b>Nur auf der Test-VHDX arbeiten.</b> Dieser Build sperrt echte Datenträger, aber prüfe vor jedem <i>Anwenden</i> im Dialog Modell und Größe. "
               "Der Name des Datenträgers muss „Msft Virtual Disk“ o. ä. sein und die Größe 2 GB.", WARN))

    s.append(p("A. Vorbereitung", H2))
    s.append(p("1. Programme: <font face='Courier'>C:\\Projects\\wparted\\build\\x64-release\\apps\\wparted-gui\\wparted.exe</font> und <font face='Courier'>...\\apps\\wparted-cli\\wparted-cli.exe</font> (daneben liegt jeweils der Helper). "
               "2. PowerShell <b>als Administrator</b> öffnen und die Test-VHDX mit Testdatei anlegen:"))
    s.append(code(r'''New-Item -ItemType Directory C:\WPartedTest -Force | Out-Null
@"
create vdisk file="C:\WPartedTest\t.vhdx" maximum=2048 type=expandable
select vdisk file="C:\WPartedTest\t.vhdx"
attach vdisk
convert gpt
create partition primary size=1500
format fs=ntfs quick label=TESTNTFS
assign letter=T
"@ | Set-Content C:\WPartedTest\make.txt
diskpart /s C:\WPartedTest\make.txt
fsutil file createnew T:\voll.bin 900000000
$b = New-Object byte[] 20MB; (New-Object Random).NextBytes($b)
[IO.File]::WriteAllBytes("T:\probe.bin", $b)
Remove-Item T:\voll.bin
Get-FileHash T:\probe.bin | Format-List Hash'''))
    s.append(p("Notiere den Hash: ______________________________________________________________ (Prüfwert H). "
               "So liegt probe.bin weit hinten im Volume, und das Verkleinern muss es verschieben."))
    s.append(checks([
        ("WParted als Administrator starten, die VHDX im Datenträger-Menü wählen, <i>Datenträger-Informationen</i> öffnen.",
         "Schnittstelle „File-backed virtual“, Größe 2,00 GiB, 1 Partition NTFS mit T:."),
        ("Den Windows-Datenträger wählen, irgendeine Datenpartition zum Verkleinern planen, <i>Anwenden</i>.",
         "Kein Knopf „Auf diesen Datenträger anwenden“. Text nennt: Es wird nichts geschrieben. Plan danach leeren."),
    ]))

    s.append(p("B. Sperren (hier darf nichts passieren)", H2))
    s.append(checks([
        ("CLI in einer normalen PowerShell, N = Nummer eines <b>echten</b> Datenträgers, plan.json = beliebiger gespeicherter Plan:<br/><font face='Courier'>wparted-cli apply-plan \\\\.\\PhysicalDriveN plan.json --execute --yes-write-to-disk</font>",
         "Fehler „real disks stay locked“ bzw. „holds the running Windows“. Kein UAC-Fenster. Nichts geschrieben."),
        ("Auf der VHDX bei eingebundenem T: die Partition zum Verkleinern planen.",
         "Fehler op.busy (eingebunden); <i>Anwenden</i> nicht möglich. Plan leeren."),
    ]))

    s.append(p("C. Einbinden und Aushängen", H2))
    s.append(checks([
        ("VHDX-Partition wählen, Menü <i>Partition &gt; Aushängen</i>, bestätigen. Im UAC-Fenster <b>Nein</b>.",
         "Meldung „administrator rights were declined“. T: bleibt da."),
        ("Zweite PowerShell öffnen und mit <font face='Courier'>cd T:\\</font> auf das Volume wechseln, dann Aushängen mit UAC <b>Ja</b>.",
         "Meldung „volume is in use“; T: bleibt da. Danach diese PowerShell schließen."),
        ("Erneut Aushängen mit UAC Ja.", "T: verschwindet im Explorer; WParted zeigt keinen Einhängepunkt mehr."),
        ("Windows-Partition C: wählen.", "<i>Einbinden</i> und <i>Aushängen</i> sind ausgegraut."),
    ]))

    s.append(p("D. Schreiben auf die VHDX (Kernprüfung)", H2))
    s.append(checks([
        ("VHDX-Partition auf <b>200 MiB</b> verkleinern, <i>Anwenden</i>, <i>Auf diesen Datenträger anwenden</i>. Der Bestätigungsdialog hat <i>Abbrechen</i> als Standard. Anwenden; UAC Ja.",
         "Dialog nennt PhysicalDriveN, Modell, Seriennummer und Größe. Ergebnis: alle Schritte ✔. Die Anzeige aktualisiert sich."),
        ("<i>Partition &gt; Einbinden</i>, Buchstabe T wählen, UAC Ja. In der Admin-PowerShell:<br/><font face='Courier'>chkdsk T:<br/>Get-FileHash T:\\probe.bin</font>",
         "chkdsk meldet keine Fehler. Hash = H. <b>Das ist die wichtigste Prüfung: Windows akzeptiert das verschobene NTFS.</b> (Hat Windows T: nach dem Anwenden schon selbst eingebunden: Einbinden überspringen und das notieren.)"),
        ("T: wieder aushängen, Partition auf 1500 MiB vergrößern, anwenden. Einbinden, chkdsk, Hash wie oben.", "Keine Fehler, Hash = H, T: ist etwa 1,5 GB groß."),
        ("Aushängen, Partition nach rechts verschieben (50 MiB), anwenden. Einbinden, chkdsk, Hash.", "Keine Fehler, Hash = H."),
        ("In freiem Bereich neue Partition NTFS, Bezeichnung WPNEU, anwenden, einbinden als U:, <font face='Courier'>chkdsk U:</font>. Dann eine Datei darauf speichern.",
         "<b>Erster chkdsk-Test von WParted-NTFS.</b> Falls chkdsk etwas ergänzt: Meldung notieren. Speichern muss gehen."),
        ("Partition aushängen, wieder nach links verschieben, anwenden und während der Verschiebung im Task-Manager <font face='Courier'>wparted-helper.exe</font> beenden. Meldung schließen, <i>Aktualisieren</i>, die VHDX wählen.",
         "WParted bietet <i>Fortsetzen</i> an (einmal je Programmstart). Fortsetzen; UAC Ja. Danach einbinden: Hash = H. Ist die Verschiebung zu schnell zum Beenden, Schritt als nicht durchführbar notieren."),
        ("U: aushängen, diese Partition löschen, anwenden.", "Partition weg, T: unberührt, Hash = H."),
    ]))

    s.append(p("E. Andere Dateisysteme auf der VHDX", H2))
    s.append(checks([
        ("Neue Partition exFAT (300 MiB), anwenden, einbinden, Testdatei speichern, aushängen, auf 100 MiB verkleinern, anwenden, einbinden.",
         "Testdatei unverändert. <font face='Courier'>chkdsk</font> ohne Fehler."),
        ("Dasselbe mit FAT32.", "Testdatei unverändert. chkdsk ohne Fehler."),
    ]))

    s.append(p("F. Oberfläche", H2))
    s.append(checks([
        ("Einstellungen &gt; Barrierefreiheit &gt; Kontrastdesigns: Design aktivieren, WParted ansehen, wieder ausschalten.",
         "Alles lesbar: Datenträgerkarte, Liste, Menüs, Dialoge. Kein Text in Hintergrundfarbe."),
        ("Neues Design: Einstellungen &gt; Anzeige &gt; Skalierung auf 150 % stellen, WParted neu starten, danach wieder zurückstellen.",
         "Runde Ecken sauber, nichts abgeschnitten, Parti nicht verpixelt."),
        ("Neues Design: helles und dunkles Design (Menü <i>Ansicht</i>) ansehen, Partition auswählen, Dialog <i>Neu</i> öffnen und abbrechen.",
         "Gut lesbar und freundlich; Warnungen bleiben klar erkennbar."),
        ("Windows-Sprache prüfen: Menü <i>Partition</i> zeigt <i>Einbinden</i>/<i>Aushängen</i> auf Deutsch.", "Deutsche Texte, nichts abgeschnitten."),
    ]))

    s.append(p("G. Aufräumen", H2))
    s.append(code(r'''@"
select vdisk file="C:\WPartedTest\t.vhdx"
detach vdisk
"@ | Set-Content C:\WPartedTest\off.txt
diskpart /s C:\WPartedTest\off.txt
Remove-Item -Recurse C:\WPartedTest'''))

    s.append(KeepTogether([
        p("H. Sicherheitsreview (Voraussetzung für echte Datenträger)", H2),
        p("Nur wenn A bis F bestanden sind. Dies kann kein Agent ersetzen; dein Masterprompt verlangt dafür einen Menschen. Lies <font face='Courier'>docs/architecture/security-architecture.md</font> (Abschnitt „Writing to disks“) und:"),
        checks([
            ("<font face='Courier'>platforms/windows/storage-api/src/disk_writer.cpp</font> gelesen: nur erwartete Schreibzugriffe.", "Verstanden und einverstanden."),
            ("<font face='Courier'>apps/wparted-helper/src/main.cpp</font> gelesen: Reihenfolge Sperre, Prüfung, Bestätigung, Schreiben.", "Verstanden und einverstanden."),
            ("Offene Punkte entschieden: unsignierter Helper in beschreibbarem Ordner; Besitzer vorhandener Journal-Dateien.", "Entscheidung notiert."),
            ("Backup-Strategie für den ersten Test auf einem <b>leeren Ersatz-Datenträger</b> festgelegt, niemals zuerst auf Produktivdaten.", "Festgelegt."),
        ]),
        Spacer(1, 6),
        p("Erst danach: Build mit <font face='Courier'>-DWPARTED_ENABLE_REAL_DISK_WRITES=ON</font>. Der Agent setzt diesen Schalter nicht von sich aus für Releases."),
    ]))

    s.append(KeepTogether([
        p("Ergebnis", H2),
        p("Abweichungen (Nr., Meldung): ______________________________________________________________"),
        Spacer(1, 12),
        p("______________________________________________________________________________________"),
        Spacer(1, 12),
        p("Datum: ______________  Geprüft von: ______________________  Echte Datenträger freigeben: ☐ ja  ☐ nein".replace("☐", "[ ]")),
    ]))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    doc = SimpleDocTemplate(str(OUT), pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm, topMargin=17 * mm, bottomMargin=17 * mm,
                            title="WParted Pruefcheckliste", author="WParted")
    doc.build(s, onFirstPage=footer, onLaterPages=footer)


if __name__ == "__main__":
    build()
