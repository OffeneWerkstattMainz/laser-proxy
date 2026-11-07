# Laser-Proxy

ESPHome-Firmware für einen ESP32, der den Lasercutter der Offenen Werkstatt Mainz in ein eigenes kleines Netz hängt und die UDP-Pakete von LightBurn/RDWorks aus dem Vereinsnetz an ihn weiterreicht.

**Warum:** Der Laser-Controller (Ryxon/Ruida KT332N) lässt sich nur mit einer IP-Adresse konfigurieren, nicht mit Subnetzmaske und Gateway. Aus einem anderen Subnetz ist er deshalb nicht erreichbar. Der Proxy steht im selben kleinen Netz wie der Laser und spricht für ihn mit dem Vereinsnetz.

```
 PC mit LightBurn                    Laser-Proxy (ESP32)                     Laser-Controller
 ─────────────────  Vereinsnetz  ──  [Uplink]          [W5500]  ── Kabel ──  KT332N
                                     z.B. 192.168.0.50  192.168.1.1          192.168.1.100
                                     (DHCP oder fest)
```

> **Status:** Ungetestet auf echter Hardware. Die CI baut die Firmware für beide Beispielkonfigurationen. Erfahrungen bitte als Issue melden.

## So funktioniert es

- Der Proxy nimmt im Vereinsnetz UDP-Pakete auf Port **50200** an und schickt sie von Port **40200** aus an den Laser, genau wie LightBurn es direkt tun würde.
- Antworten des Lasers gehen an den Rechner zurück, der zuletzt gesendet hat.
- Port **50207/40207** (laut Community-Dokumentation für Bedienfeld/Jog) wird genauso weitergeleitet.
- Es wird nichts geroutet. Das Lasernetz bleibt abgeschottet, der Laser selbst sieht nur den Proxy.
- Wie beim Laser direkt sollte immer nur ein Rechner gleichzeitig mit ihm arbeiten.

## Hardware

Zwei Bauformen sind als Beispielkonfiguration enthalten:

| Datei | Board | Vereinsnetz | Laser |
|---|---|---|---|
| [`laser-proxy.yaml`](laser-proxy.yaml) | WT32-ETH01 (ESP32 + LAN8720) | eingebauter Ethernet-Port | zusätzliches W5500-Modul |
| [`laser-proxy-s3-wlan.yaml`](laser-proxy-s3-wlan.yaml) | Waveshare ESP32-S3-ETH | WLAN | eingebauter Ethernet-Port (W5500) |

Für die Werkstatt ist die Variante mit zwei Kabeln die robustere: Der Laser bestätigt jedes Paket einzeln, WLAN-Aussetzer bremsen Uploads deshalb spürbar.

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

### Waveshare ESP32-S3-ETH

Keine Verdrahtung nötig. Die W5500-Pins stammen aus dem Waveshare-Wiki (MISO 12, MOSI 11, SCLK 13, CS 14, INT 10, RST 9). Strom über USB-C, denn am Laser-Port kommt kein PoE an.

Variante mit zwei Kabeln auf diesem Board (PoE-Port ins Vereinsnetz, zweites W5500-Modul zum Laser): Den eingebauten W5500 übernimmt dann die `ethernet:`-Komponente (`type: W5500`), das zweite Modul `laser_proxy`. Es kann am selben SPI-Bus hängen (gleiche SCLK/MOSI/MISO-Pins und `spi_host` wie die `ethernet:`-Komponente, eigener CS- und INT-Pin) oder an einem eigenen Bus. Freie Pins vorher anhand des Schaltplans prüfen, das Wiki nennt sie nicht vollständig.

## Einrichtung

1. **Laser-Controller** am Bedienfeld auf `192.168.1.100` stellen (Werkseinstellung von Ruida). Das Lasernetz darf sich nicht mit dem Vereinsnetz überschneiden. Falls das Vereinsnetz selbst `192.168.1.0/24` nutzt, `local_ip` und `laser_ip` auf ein anderes Netz ändern, z.B. `10.77.0.1` und `10.77.0.100`.
2. **Secrets:** `secrets.example.yaml` nach `secrets.yaml` kopieren und eigene Werte eintragen.
3. **Flashen:** `esphome run laser-proxy.yaml`. Beim ersten Mal per USB (WT32-ETH01: IO0 beim Einschalten auf GND halten), danach per OTA übers Netz. Alternativ die fertige `firmware.factory.bin` aus dem CI-Lauf mit [web.esphome.io](https://web.esphome.io) flashen.
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
    spi_host: SPI2           # optional, SPI2 oder SPI3
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
