# Laser-Proxy

ESPHome-Firmware für einen ESP32, der den Lasercutter der Offenen Werkstatt Mainz in ein eigenes kleines Netz hängt und die UDP-Pakete von LightBurn/RDWorks aus dem Vereinsnetz an ihn weiterreicht.

**Warum:** Der Laser-Controller (Ryxon/Ruida KT332N) lässt sich nur mit einer IP-Adresse konfigurieren, nicht mit Subnetzmaske und Gateway. Aus einem anderen Subnetz ist er deshalb nicht erreichbar. Der Proxy steht im selben kleinen Netz wie der Laser und spricht für ihn mit dem Vereinsnetz.

```
 PC mit LightBurn                    Laser-Proxy (ESP32)                     Laser-Controller
 ─────────────────  Vereinsnetz  ──  [Uplink]          [W5500]  ── Kabel ──  KT332N
                                     z.B. 192.168.0.50  192.168.1.1          192.168.1.100
                                     (DHCP oder fest)
```

> **Status:** Ungetestet auf echter Hardware. Die CI baut die Firmware für alle Beispielkonfigurationen. Erfahrungen bitte als Issue melden.

## So funktioniert es

- Der Proxy nimmt im Vereinsnetz UDP-Pakete auf Port **50200** an und schickt sie von Port **40200** aus an den Laser, genau wie LightBurn es direkt tun würde.
- Antworten des Lasers gehen an den Rechner zurück, der zuletzt gesendet hat.
- Port **50207/40207** (laut Community-Dokumentation für Bedienfeld/Jog) wird genauso weitergeleitet.
- Es wird nichts geroutet. Das Lasernetz bleibt abgeschottet, der Laser selbst sieht nur den Proxy.
- Wie beim Laser direkt sollte immer nur ein Rechner gleichzeitig mit ihm arbeiten.

## Hardware

Drei Bauformen sind als Beispielkonfiguration enthalten:

| Datei | Board | Vereinsnetz | Laser |
|---|---|---|---|
| [`laser-proxy.yaml`](laser-proxy.yaml) | WT32-ETH01 (ESP32 + LAN8720) | eingebauter Ethernet-Port | zusätzliches W5500-Modul |
| [`laser-proxy-s3-eth.yaml`](laser-proxy-s3-eth.yaml) | Waveshare ESP32-S3-ETH | eingebauter Ethernet-Port (W5500, PoE möglich) | zusätzliches W5500-Modul |
| [`laser-proxy-s3-wlan.yaml`](laser-proxy-s3-wlan.yaml) | Waveshare ESP32-S3-ETH | WLAN | eingebauter Ethernet-Port (W5500) |

Für die Werkstatt sind die Varianten mit zwei Kabeln die robusteren: Der Laser bestätigt jedes Paket einzeln, WLAN-Aussetzer bremsen Uploads deshalb spürbar.

### WT32-ETH01 + W5500-Modul

Material: WT32-ETH01, W5500-Modul (3,3 V), 5-V-Netzteil, zum ersten Flashen ein USB-Seriell-Adapter mit 3,3 V.

| W5500-Modul | WT32-ETH01 | Hinweis |
|---|---|---|
| 3V3 | 3V3 | Das Modul zieht bis ca. 150 mA. Bei Problemen eigenen 3,3-V-Regler nehmen. |
| GND | GND | |
| SCLK | IO14 | |
| MOSI | IO15 | Strapping-Pin, als Ausgang unkritisch |
| MISO | IO35 | nur Eingang |
| CS / SCS | IO4 | |
| INT | IO36 | nur Eingang, 10-kΩ-Pull-up nach 3V3 empfohlen |
| RST | IO32 | optional, sonst `reset_pin` weglassen |

Nicht verwenden: IO0 (RMII-Takt), IO2 und IO12 (Strapping), IO16 bis IO27 außer den oben genannten (Ethernet-PHY).

### Waveshare ESP32-S3-ETH + W5500-Modul

Der eingebaute Port geht ins Vereinsnetz und wird von der `ethernet:`-Komponente betrieben (`type: W5500`, Pins laut Waveshare-Wiki: MISO 12, MOSI 11, SCLK 13, CS 14, INT 10, RST 9). Das zusätzliche W5500-Modul hängt am Laser. Die SPI-Pins des eingebauten W5500 liegen nicht auf der Stiftleiste, das Modul bekommt deshalb einen eigenen SPI-Bus (`spi_host: SPI3`, die `ethernet:`-Komponente nutzt SPI2).

Material: Waveshare ESP32-S3-ETH, W5500-Modul (3,3 V), Strom über USB-C oder über PoE, falls das PoE-Modul aufgesteckt ist und der Switch PoE liefert.

Alle Anschlüsse liegen auf der linken Stiftleiste (Pin 25 bis 36, Nummerierung laut Waveshare-Pinout):

| W5500-Modul | ESP32-S3-ETH | Pin | Hinweis |
|---|---|---|---|
| 3V3 | 3V3 | 36 | Das Modul zieht bis ca. 150 mA. Bei Problemen eigenen 3,3-V-Regler nehmen. |
| GND | GND | 33 | |
| SCLK | GPIO17 | 34 | |
| MOSI | GPIO16 | 32 | |
| MISO | GPIO18 | 31 | |
| CS / SCS | GPIO15 | 29 | |
| INT | GPIO2 | 26 | interner Pull-up reicht |
| RST | GPIO1 | 25 | optional, sonst `reset_pin` weglassen |

Die genutzten Pins sind sonst nur mit dem Kamera-Anschluss verbunden, der hier frei bleibt. Nicht verwenden: GPIO0, GPIO3, GPIO45 und GPIO46 (Strapping), GPIO19 und GPIO20 (USB), GPIO33 bis GPIO37 (laut Wiki intern belegt), GPIO43 und GPIO44 (UART0), GPIO21 (RGB-LED), GPIO4 bis GPIO7 (SD-Karte).

### Waveshare ESP32-S3-ETH mit WLAN

Keine Verdrahtung nötig. Der eingebaute Port hängt am Laser, ins Vereinsnetz geht es per WLAN. Strom über USB-C, denn am Laser-Port kommt kein PoE an.

## Einrichtung

1. **Laser-Controller** am Bedienfeld auf `192.168.1.100` stellen (Werkseinstellung von Ruida). Das Lasernetz darf sich nicht mit dem Vereinsnetz überschneiden. Falls das Vereinsnetz selbst `192.168.1.0/24` nutzt, `local_ip` und `laser_ip` auf ein anderes Netz ändern, z.B. `10.77.0.1` und `10.77.0.100`.
2. **Secrets:** `secrets.example.yaml` nach `secrets.yaml` kopieren und eigene Werte eintragen.
3. **Flashen:** `esphome run laser-proxy.yaml`. Beim ersten Mal per USB (WT32-ETH01: IO0 beim Einschalten auf GND halten; Waveshare: USB-C, falls nötig BOOT-Taste beim Einstecken halten), danach per OTA übers Netz. Alternativ die fertige `firmware.factory.bin` aus dem CI-Lauf mit [web.esphome.io](https://web.esphome.io) flashen.
4. **Vereinsnetz:** Im Router eine feste DHCP-Reservierung für `laser-proxy` anlegen oder unter `ethernet:`/`wifi:` eine `manual_ip` setzen.
5. **LightBurn:** Gerät vom Typ Ruida mit Verbindung über Ethernet/UDP anlegen und als IP-Adresse die des Proxys im Vereinsnetz eintragen. RDWorks entsprechend.

Status und Zähler stehen auf der Weboberfläche des Proxys (`http://laser-proxy.local/`) und in Home Assistant, falls eingebunden.

Außerhalb dieses Repos, z.B. im ESPHome-Dashboard von Home Assistant, die Komponente so einbinden:

```yaml
external_components:
  - source: github://OffeneWerkstattMainz/laser-proxy@main
    components: [laser_proxy]
```

## Konfiguration

```yaml
laser_proxy:
  w5500:
    clk_pin: GPIO14
    mosi_pin: GPIO15
    miso_pin: GPIO35
    cs_pin: GPIO4
    interrupt_pin: GPIO36    # optional, ohne INT wird gepollt
    reset_pin: GPIO32        # optional
    clock_speed: 12MHz       # optional
    polling_interval: 10ms   # optional, nur ohne interrupt_pin
    spi_host: SPI2           # optional, SPI2 oder SPI3 (SPI3, wenn ethernet: einen W5500 an SPI2 betreibt)
  local_ip: 192.168.1.1      # Adresse des Proxys im Lasernetz
  subnet: 255.255.255.0      # optional
  laser_ip: 192.168.1.100    # Adresse des Laser-Controllers
  ports:                     # optional, das ist der Standard
    - laser: 50200
      host: 40200
    - laser: 50207
      host: 40207
  allowed_network: 0.0.0.0/0 # optional, z.B. 192.168.0.0/24
  reply_to_host_port: false  # optional, siehe unten
  isolate_ethernet_events: true  # optional, siehe unten
  update_interval: 10s       # optional, für die Zähler
  # optionale Sensoren:
  laser_link: { name: Laser verbunden }
  client: { name: Aktiver Client }
  packets_to_laser: { name: Pakete zum Laser }
  packets_from_laser: { name: Pakete vom Laser }
  packets_dropped: { name: Verworfene Pakete }
```

- `reply_to_host_port`: Antworten gehen normalerweise an Adresse und Port, von denen der Rechner gesendet hat. LightBurn und RDWorks senden ohnehin von 40200. Nur einschalten, falls ein Programm von einem anderen Port sendet, die Antwort aber auf 40200 erwartet.
- `isolate_ethernet_events`: Die `ethernet:`-Komponente von ESPHome geht von genau einem Ethernet-Port aus und würde Link-Ereignisse des Laser-Ports als ihre eigenen werten (Laser aus = Uplink weg). Die Komponente fängt diese Ereignisse deshalb ab. Dafür greift sie auf einen internen Handler von ESPHome zu. Falls eine neue ESPHome-Version deshalb nicht mehr baut, `false` setzen; dann startet die `ethernet:`-Komponente beim Ausschalten des Lasers kurz ihre DHCP-Anfrage neu, sonst passiert nichts. Mit WLAN als Uplink spielt die Option keine Rolle.

## Entwicklung

```bash
pip install esphome
cp secrets.example.yaml secrets.yaml
esphome compile laser-proxy.yaml
```

Die Komponente liegt unter [`components/laser_proxy`](components/laser_proxy). Voraussetzung ist das ESP-IDF-Framework (`esp32: framework: type: esp-idf`).
