"""ESPHome-Komponente: UDP-Relay zwischen Vereinsnetz und einem Ruida-Laser-Controller.

Der Laser haengt an einem eigenen W5500-Ethernet-Port mit eigenem kleinen Netz.
Das Uplink-Netz (Vereinsnetz) stellt die normale ESPHome-``ethernet:``- oder
``wifi:``-Komponente.
"""

import ipaddress

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
)
from esphome.core import TimePeriodMilliseconds

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["binary_sensor", "sensor", "text_sensor"]

CONF_W5500 = "w5500"
CONF_CLK_PIN = "clk_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_MISO_PIN = "miso_pin"
CONF_CS_PIN = "cs_pin"
CONF_INTERRUPT_PIN = "interrupt_pin"
CONF_RESET_PIN = "reset_pin"
CONF_CLOCK_SPEED = "clock_speed"
CONF_POLLING_INTERVAL = "polling_interval"
CONF_SPI_HOST = "spi_host"
CONF_LOCAL_IP = "local_ip"
CONF_SUBNET = "subnet"
CONF_LASER_IP = "laser_ip"
CONF_PORTS = "ports"
CONF_LASER = "laser"
CONF_HOST = "host"
CONF_ALLOWED_NETWORK = "allowed_network"
CONF_REPLY_TO_HOST_PORT = "reply_to_host_port"
CONF_ISOLATE_ETHERNET_EVENTS = "isolate_ethernet_events"
CONF_LASER_LINK = "laser_link"
CONF_CLIENT = "client"
CONF_PACKETS_TO_LASER = "packets_to_laser"
CONF_PACKETS_FROM_LASER = "packets_from_laser"
CONF_PACKETS_DROPPED = "packets_dropped"

laser_proxy_ns = cg.esphome_ns.namespace("laser_proxy")
LaserProxy = laser_proxy_ns.class_("LaserProxy", cg.PollingComponent)

SPI_HOSTS = {"SPI2": 2, "SPI3": 3}

DEFAULT_PORTS = [
    {CONF_LASER: 50200, CONF_HOST: 40200},  # Ruida-Datenkanal
    {CONF_LASER: 50207, CONF_HOST: 40207},  # Bedienfeld/Jog
]


def _ipv4(value):
    value = cv.string_strict(value)
    try:
        ipaddress.IPv4Address(value)
    except ValueError as err:
        raise cv.Invalid(f"Keine gueltige IPv4-Adresse: {value}") from err
    return value


def _ipv4_network(value):
    value = cv.string_strict(value)
    try:
        return str(ipaddress.IPv4Network(value, strict=False))
    except ValueError as err:
        raise cv.Invalid(f"Kein gueltiges Netz (z.B. 192.168.0.0/24): {value}") from err


def _counter_schema(icon):
    return sensor.sensor_schema(
        accuracy_decimals=0,
        icon=icon,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


W5500_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_MOSI_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_MISO_PIN): pins.internal_gpio_input_pin_number,
        cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_INTERRUPT_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_RESET_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_CLOCK_SPEED, default="12MHz"): cv.All(
            cv.frequency, cv.int_range(min=int(1e6), max=int(40e6))
        ),
        cv.Optional(CONF_POLLING_INTERVAL, default="10ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(
                min=TimePeriodMilliseconds(milliseconds=1),
                max=TimePeriodMilliseconds(milliseconds=1000),
            ),
        ),
        cv.Optional(CONF_SPI_HOST, default="SPI2"): cv.one_of(*SPI_HOSTS, upper=True),
    }
)

PORT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_LASER): cv.port,
        cv.Required(CONF_HOST): cv.port,
    }
)


def _validate(config):
    local = ipaddress.IPv4Address(config[CONF_LOCAL_IP])
    laser = ipaddress.IPv4Address(config[CONF_LASER_IP])
    try:
        net = ipaddress.IPv4Network(f"{local}/{config[CONF_SUBNET]}", strict=False)
    except ValueError as err:
        raise cv.Invalid(f"Ungueltige Subnetzmaske: {config[CONF_SUBNET]}") from err
    if laser not in net:
        raise cv.Invalid(
            f"{CONF_LASER_IP} ({laser}) liegt nicht im Lasernetz {net} von {CONF_LOCAL_IP}"
        )
    if laser == local:
        raise cv.Invalid(f"{CONF_LASER_IP} und {CONF_LOCAL_IP} muessen verschieden sein")
    lasers = [p[CONF_LASER] for p in config[CONF_PORTS]]
    hosts = [p[CONF_HOST] for p in config[CONF_PORTS]]
    if len(set(lasers)) != len(lasers) or len(set(hosts)) != len(hosts):
        raise cv.Invalid(f"Doppelte Ports in {CONF_PORTS}")
    if not 1 <= len(config[CONF_PORTS]) <= 4:
        raise cv.Invalid(f"{CONF_PORTS} braucht 1 bis 4 Eintraege")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LaserProxy),
            cv.Required(CONF_W5500): W5500_SCHEMA,
            cv.Required(CONF_LOCAL_IP): _ipv4,
            cv.Optional(CONF_SUBNET, default="255.255.255.0"): _ipv4,
            cv.Required(CONF_LASER_IP): _ipv4,
            cv.Optional(CONF_PORTS, default=DEFAULT_PORTS): cv.ensure_list(PORT_SCHEMA),
            cv.Optional(CONF_ALLOWED_NETWORK, default="0.0.0.0/0"): _ipv4_network,
            cv.Optional(CONF_REPLY_TO_HOST_PORT, default=False): cv.boolean,
            cv.Optional(CONF_ISOLATE_ETHERNET_EVENTS, default=True): cv.boolean,
            cv.Optional(CONF_LASER_LINK): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_CLIENT): text_sensor.text_sensor_schema(
                icon="mdi:laptop",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PACKETS_TO_LASER): _counter_schema("mdi:upload-network"),
            cv.Optional(CONF_PACKETS_FROM_LASER): _counter_schema("mdi:download-network"),
            cv.Optional(CONF_PACKETS_DROPPED): _counter_schema("mdi:close-network"),
        }
    ).extend(cv.polling_component_schema("10s")),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    w5500 = config[CONF_W5500]
    cg.add(
        var.set_spi_pins(
            w5500[CONF_CLK_PIN],
            w5500[CONF_MOSI_PIN],
            w5500[CONF_MISO_PIN],
            w5500[CONF_CS_PIN],
        )
    )
    cg.add(var.set_interrupt_pin(w5500.get(CONF_INTERRUPT_PIN, -1)))
    cg.add(var.set_reset_pin(w5500.get(CONF_RESET_PIN, -1)))
    cg.add(var.set_clock_speed(int(w5500[CONF_CLOCK_SPEED])))
    cg.add(var.set_polling_interval(w5500[CONF_POLLING_INTERVAL].total_milliseconds))
    cg.add(var.set_spi_host(SPI_HOSTS[w5500[CONF_SPI_HOST]]))

    cg.add(var.set_local_ip(config[CONF_LOCAL_IP]))
    cg.add(var.set_subnet(config[CONF_SUBNET]))
    cg.add(var.set_laser_ip(config[CONF_LASER_IP]))
    for pair in config[CONF_PORTS]:
        cg.add(var.add_port_pair(pair[CONF_LASER], pair[CONF_HOST]))

    allowed = ipaddress.IPv4Network(config[CONF_ALLOWED_NETWORK])
    cg.add(var.set_allowed_network(str(allowed.network_address), str(allowed.netmask)))
    cg.add(var.set_reply_to_host_port(config[CONF_REPLY_TO_HOST_PORT]))

    if config[CONF_ISOLATE_ETHERNET_EVENTS]:
        cg.add_define("LASER_PROXY_ISOLATE_ETH_EVENTS")

    if conf := config.get(CONF_LASER_LINK):
        cg.add(var.set_link_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_CLIENT):
        cg.add(var.set_client_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_PACKETS_TO_LASER):
        cg.add(var.set_to_laser_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_PACKETS_FROM_LASER):
        cg.add(var.set_from_laser_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_PACKETS_DROPPED):
        cg.add(var.set_dropped_sensor(await sensor.new_sensor(conf)))

    # Treiber fuer den W5500 im ESP-IDF einschalten
    add_idf_sdkconfig_option("CONFIG_ETH_USE_SPI_ETHERNET", True)
    add_idf_sdkconfig_option("CONFIG_ETH_SPI_ETHERNET_W5500", True)
