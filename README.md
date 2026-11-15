# E-Auto Smart Ladestation

Smart-Charging-Projekt für den Mazda MX-30 mit einer ESP32-Basisstation, Fahrzeug-Node und Shelly-Steuerung.

## Firmwarestand

| Komponente | Stand aus den verfügbaren Quell-Dateien | Hardware |
| --- | --- | --- |
| Basis | v2.11.1 (06.11.2026) | ESP32-WROOM-32D |
| Node | v2.11.2 (06.11.2026) | ESP32-C3 + MMA8452Q |

## Projektstruktur

- `MX30_Basis/MX30_Basis.ino` – WLAN, UDP-Empfang, Web-UI und Shelly-Pro-1PM-Steuerung
- `MX30_Auto/MX30_Auto.ino` – Bewegungssensor, WLAN-Meldungen, Deep-Sleep
- In jedem Sketch-Verzeichnis: `local_secret.example.h` als Vorlage für `local_secret.h`

## Arduino IDE

1. Arduino-ESP32 **3.x** installieren. Für die Basis zusätzlich **ArduinoJson 7** installieren.
2. In **beiden** Sketch-Ordnern jeweils `local_secret.example.h` nach `local_secret.h` kopieren.
3. In beiden `local_secret.h`-Dateien **denselben individuellen, ausreichend langen UDP-Schlüssel** eintragen. Beide müssen übereinstimmen, sonst kommunizieren Basis und Node nicht.
4. `MX30_Basis/MX30_Basis.ino` bzw. `MX30_Auto/MX30_Auto.ino` in Arduino IDE öffnen, Board auswählen, kompilieren und flashen.

**Achtung zur bestehenden Installation:** Die ursprünglichen Firmware-Dateien enthielten bereits einen gemeinsamen Schlüssel. Um die Kommunikation mit einem noch nicht aktualisierten Gerät zu erhalten, muss vorübergehend der gleiche Schlüssel wie in der bisherigen Installation verwendet werden. Besser: einen neuen, individuellen Schlüssel wählen und **beide** Geräte mit diesem Schlüssel aktualisieren. Niemals einen echten Schlüssel einchecken.

## Sicherheit und Betrieb

Dieses Repository ist zum Zeitpunkt des Imports **öffentlich**. Deshalb wurde der fest im Ausgangscode hinterlegte gemeinsame UDP-Schlüssel aus den veröffentlichten Sketches entfernt. `local_secret.h` ist über `.gitignore` ausgeschlossen. Die UDP-Kommunikation mit dem Schlüssel ist keine verschlüsselte oder starke Authentifizierung.

Die Setup-Access-Points der importierten Firmware sind **offen** – nur in einer abgesicherten Umgebung betreiben. Bestehende Schutzlogik für den Ladebetrieb unverändert lassen. Netzspannungsinstallationen und Schütz-/Shelly-Verdrahtung nur fachgerecht und nach geltenden Vorschriften ausführen.

Die importierten Quellstände wurden **nicht neu kompiliert oder auf der Hardware getestet**.
