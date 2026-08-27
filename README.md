*Deutsch | [English](README.en.md)*

# m5dial_clock_sbb

Ein einziges, schlankes ESPHome/LVGL-Widget: die klassische **SBB-Bahnhofsuhr**
(Schweizer Bahnhofsuhr, entworfen 1944 von Hans Hilfiker) als analoge Uhr auf
einem M5Dial (oder jedem anderen ESP32 + LVGL-Display). Kein Baukasten mit
fünf Stilen mehr, keine Multi-Display-Synchronisation, keine Choreographie-
Modi - nur diese eine Uhr, dafür mit echter Zeigerkinematik, Nachtmodus,
Datum und Temperatur eingebaut.

## Warum neu geschrieben statt geforkt

Dieses Repo begann als Idee, `tuct/esphome-lvgl-clock` zu forken und
zurechtzustutzen. Zwei Gründe dagegen:

1. **Keine Lizenz im Original-Repo** - ohne `LICENSE`-Datei gilt "alle Rechte
   vorbehalten"; Code daraus kopieren und umbauen wäre rechtlich nicht
   abgesichert gewesen.
2. Die Komponente dort zeichnet laut eigenem README **"continuous sweep - no
   ticking or stop-to-go pause"** - genau die Mechanik, die eine echte SBB-Uhr
   ausmacht, fehlte also komplett und musste ohnehin neu gebaut werden.

Der Code hier ist eine unabhängige Neuentwicklung; übernommen wurde nichts
außer der öffentlichen, generischen LVGL-9-Zeichentechnik (`lv_draw_line`,
`lv_draw_rect`, `lv_draw_label` auf einem Canvas-Layer) - das ist die einzige
Art, wie *irgendjemand* mit LVGL eine Uhr auf ein Canvas zeichnet, unabhängig
vom Autor.

## Die Stop2Go-Mechanik

Online verifiziert (SBB/Mondaine, "Stop2Go"-Erklärung): Der rote
Sekundenzeiger läuft in **58 Sekunden** einmal um das Zifferblatt, bleibt dann
**2 Sekunden auf 12 stehen** - er wartet auf den elektrischen Impuls der
nächsten Minute - und in genau diesem Moment springt der Minutenzeiger einen
Schritt vor. Der Stundenzeiger läuft dagegen mechanisch/kontinuierlich mit.

Diese Komponente bildet das nach:

- Sekundenzeiger: linearer Sweep über `second_hand_sweep` (Default `58s`),
  danach Parkposition auf 12 für den Rest der Minute.
- Minutenzeiger: springt diskret, kein Nachlaufen.
- Stundenzeiger: läuft kontinuierlich mit den Minuten/Sekunden mit.

`second_hand_sweep` ist konfigurierbar (1-59s), falls eine Anwendung die Pause
über- oder untertreiben will - der Default ist der recherchierte Realwert.

## Features

- **Skaliert**: nur `width`/`height` ändern, die komplette Geometrie (Zeiger,
  Ticks, Nabe, Zeitzone-Skalierung) ist proportional zum Radius. Schrift für
  Datum/Temperatur bleibt Rastergröße - bei stark abweichenden Größen den
  `date_font`/`temperature_font` selbst passend wählen.
- **Nachtmodus**: `id(clock).set_night_mode(true)` (Laufzeit, kein YAML-Reload
  nötig) tauscht Vordergrund/Hintergrund (Zifferblatt, Ticks, Stunden-/
  Minutenzeiger, Datum/Temperatur-Text) - Schwarz/Weiß invertiert. Der
  Sekundenzeiger hat seine eigene Farbe (Default Rot, `E74C3C`) und bleibt in
  beiden Modi unverändert, genau wie beim Original.
- **Datumszeile** unten im Zifferblatt (`show_date: true`), formatiert aus
  `time_id` - kein separates `interval:`/`lambda:`-Konstrukt mehr nötig.
- **Temperaturzeile** oben (`show_temperature: true` +
  `temperature_sensor_id:`) - wird direkt vom Sensor gelesen und mitgezeichnet.
- Nur eine Canvas-Rendering-Pass pro Frame (Zifferblatt → Ticks → Text →
  Zeiger → Nabe), daher **kein `transparent: true` mehr nötig**, nur um Text
  unter die Zeiger zu bekommen (wie im alten `clock.yaml`) - das spart die
  Hälfte des Canvas-RAMs (RGB565 statt ARGB8888), außer man will wirklich
  einen Hintergrund durchscheinen lassen.

## Nutzung

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/CzarofAK/m5dial_clock_sbb
      ref: main
    components: [sbb_clock]

sbb_clock:  # Marker-Key, ohne den lädt ESPHome die Komponente nicht

color:
  - id: col_ink
    hex: FFFFFF
  - id: col_bg
    hex: "000000"
  - id: col_second
    hex: E74C3C

lvgl:
  widgets:
    - sbb_clock:
        id: clock_face
        time_id: homeassistant_time
        width: 240
        height: 240
        align: CENTER
        foreground: col_ink
        background: col_bg
        second_hand_color: col_second
        show_date: true
        show_temperature: true
        temperature_sensor_id: s_aussentemp
```

Nachtmodus per Automation umschalten:

```yaml
sun:
  # ...

time:
  - platform: homeassistant
    id: homeassistant_time
    on_time:
      - seconds: 0
        minutes: 0
        then:
          - lambda: |-
              id(clock_face).set_night_mode(id(my_sun).is_below_horizon());
```

(oder gebunden an einen Lux-Sensor, einen `binary_sensor`, was auch immer
die jeweilige Anwendung als "Nacht" definiert - die Komponente kennt nur den
Zustand, nicht die Regel dahinter.)

## Konfigurationsreferenz

| Key | Typ | Default | Bedeutung |
|---|---|---|---|
| `width`, `height` | int | *(Pflicht)* | Canvas-Größe in px |
| `time_id` | id | *(Pflicht)* | ein `time:`-Element |
| `show_seconds` | bool | `true` | Sekundenzeiger zeichnen |
| `second_hand_sweep` | Zeit | `58s` | Sweep-Dauer; Rest von 60s = Pause auf 12 |
| `render_interval` | Zeit | `100ms` | Redraw-Takt |
| `foreground` | Color-ID | Weiß | "Tinte": Zeiger, Ticks, Text |
| `background` | Color-ID | Schwarz | Canvas-Hintergrund |
| `second_hand_color` | Color-ID | `E74C3C` | unabhängig von Nachtmodus |
| `transparent` | bool | `false` | ARGB8888-Canvas statt RGB565 |
| `show_ticks` | bool | `true` | Minuten-/Stunden-Ticks |
| `show_face` | bool | `false` | gefüllter Zifferblatt-Kreis |
| `face_color` | Color-ID | `background` | nur mit `show_face` |
| `night_mode` | bool | `false` | Startwert; zur Laufzeit per `set_night_mode()` |
| `show_date` | bool | `false` | Datumszeile unten |
| `date_font` | Font | `montserrat_16` | |
| `show_temperature` | bool | `false` | Temperaturzeile oben |
| `temperature_sensor_id` | id | - | Pflicht, sobald `show_temperature: true` |
| `temperature_font` | Font | `montserrat_16` | |

## Stand der Verifizierung

- Stop2Go-Mechanik: online gegen die offizielle SBB/Mondaine-Erklärung
  geprüft (58s Sweep + 2s Pause, Minutensprung während der Pause).
- `ESPTime::day_of_week` (Sonntag = 1): direkt aus ESPHomes eigenem
  `esphome/core/time.h` verifiziert.
- Jede verwendete LVGL-Funktion/Struct (`lv_draw_line_dsc_t`,
  `lv_draw_rect_dsc_t`, `lv_draw_label_dsc_t`, `lv_canvas_*`,
  `lv_draw_buf_*`) gegen den echten LVGL-9.5.0-Quellcode geprüft (die von
  ESPHome gepinnte Version).
- Die Python-Config-Seite (`components/sbb_clock/__init__.py`) wurde mit
  `esphome config` gegen eine echte Testkonfiguration erfolgreich validiert.
- **Nicht möglich in dieser Sitzung:** ein vollständiger `esphome compile` -
  das lädt Pakete von PlatformIOs Registry, die vom Netzwerk dieser Sitzung
  aus policy-seitig blockiert ist. Vor dem Flashen auf echte Hardware bitte
  einmal `esphome compile` lokal laufen lassen.
