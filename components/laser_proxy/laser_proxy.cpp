#include "laser_proxy.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "lwip/sockets.h"

#if defined(LASER_PROXY_ISOLATE_ETH_EVENTS) && defined(USE_ETHERNET)
#include "esphome/components/ethernet/ethernet_component.h"
#endif

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
#error "laser_proxy braucht ESP-IDF 5.x (esp32: framework: type: esp-idf)"
#endif

namespace esphome {
namespace laser_proxy {

static const char *const TAG = "laser_proxy";
static constexpr size_t BUF_SIZE = 2048;

#if defined(LASER_PROXY_ISOLATE_ETH_EVENTS) && defined(USE_ETHERNET)
namespace {
// Die ESPHome-Ethernet-Komponente meldet ihren statischen Handler fuer *alle*
// ETH_EVENTs an und prueft nicht, von welchem Port ein Ereignis kommt. Ohne
// Gegenmassnahme wuerde ein Link-Verlust am Laser-Port als Verlust des Uplinks
// gewertet. Wir melden ihren Handler deshalb ab und reichen ihm nur noch
// Uplink-Ereignisse weiter.
//
// Der Handler ist protected und die Klasse final. An seine Adresse kommen wir
// ueber eine explizite Template-Instanziierung, bei der laut C++-Standard keine
// Zugriffspruefung stattfindet ([temp.spec.general]). Aendert ESPHome den
// Handler, schlaegt das hier beim Bauen fehl; dann isolate_ethernet_events: false.
template<esp_event_handler_t Handler> struct EthHandlerAccess {
  friend esp_event_handler_t esphome_eth_event_handler() { return Handler; }
};
esp_event_handler_t esphome_eth_event_handler();
template struct EthHandlerAccess<&ethernet::EthernetComponent::eth_event_handler>;
}  // namespace
#endif

static std::string ip_to_string(uint32_t ip_n) {
  const auto *b = reinterpret_cast<const uint8_t *>(&ip_n);
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return buf;
}

static int open_udp(uint32_t addr_n, uint16_t port) {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0)
    return -1;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = addr_n;
  if (bind(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(s);
    return -1;
  }
  return s;
}

void LaserProxy::setup() {
  ESP_LOGCONFIG(TAG, "Richte Laser-Proxy ein...");

  this->local_ip_n_ = esp_ip4addr_aton(this->local_ip_.c_str());
  this->subnet_n_ = esp_ip4addr_aton(this->subnet_.c_str());
  this->laser_ip_n_ = esp_ip4addr_aton(this->laser_ip_.c_str());
  this->allowed_net_n_ = esp_ip4addr_aton(this->allowed_net_.c_str());
  this->allowed_mask_n_ = esp_ip4addr_aton(this->allowed_mask_.c_str());

  // Normalerweise schon von ethernet/wifi erledigt; doppelte Aufrufe sind harmlos.
  esp_netif_init();
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Event-Loop: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  if (!this->init_w5500_() || !this->init_netif_()) {
    this->mark_failed();
    return;
  }
  this->hook_eth_events_();

  err = esp_eth_start(this->eth_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "W5500 laesst sich nicht starten: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  if (!this->open_sockets_()) {
    this->mark_failed();
    return;
  }

  if (xTaskCreate(&LaserProxy::relay_task_, "laser_relay", 4096, this, 5, &this->task_) != pdPASS) {
    ESP_LOGE(TAG, "Relay-Task konnte nicht gestartet werden");
    this->mark_failed();
    return;
  }
}

bool LaserProxy::init_w5500_() {
  esp_err_t err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "GPIO-ISR-Dienst: %s", esp_err_to_name(err));
    return false;
  }

  spi_host_device_t host = this->spi_host_ == 3 ? SPI3_HOST : SPI2_HOST;
  spi_bus_config_t buscfg{};
  buscfg.mosi_io_num = this->mosi_pin_;
  buscfg.miso_io_num = this->miso_pin_;
  buscfg.sclk_io_num = this->clk_pin_;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  err = spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO);
  if (err == ESP_ERR_INVALID_STATE) {
    // Bus wird schon genutzt, z.B. vom W5500 der ethernet-Komponente. Dann
    // haengen wir uns mit eigenem CS an denselben Bus (gleiche Pins noetig).
    ESP_LOGI(TAG, "SPI%d ist bereits eingerichtet, W5500 wird am selben Bus betrieben", this->spi_host_);
  } else if (err != ESP_OK) {
    ESP_LOGE(TAG, "SPI-Bus: %s", esp_err_to_name(err));
    return false;
  }

  spi_device_interface_config_t devcfg{};
  devcfg.mode = 0;
  devcfg.clock_speed_hz = this->clock_speed_;
  devcfg.queue_size = 20;
  devcfg.spics_io_num = this->cs_pin_;

  eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(host, &devcfg);
  w5500_config.int_gpio_num = this->interrupt_pin_;
  if (this->interrupt_pin_ < 0) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 2)
    w5500_config.poll_period_ms = this->polling_interval_;
#else
    ESP_LOGE(TAG, "Ohne interrupt_pin braucht der W5500 ESP-IDF >= 5.1.2");
    return false;
#endif
  }

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.phy_addr = 1;
  phy_config.reset_gpio_num = this->reset_pin_;

  esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
  if (mac == nullptr || phy == nullptr) {
    ESP_LOGE(TAG, "W5500-Treiber konnte nicht angelegt werden");
    return false;
  }

  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  err = esp_eth_driver_install(&eth_config, &this->eth_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "W5500 antwortet nicht (%s). Verdrahtung und Pins pruefen.", esp_err_to_name(err));
    return false;
  }

  // Der W5500 hat keine eigene MAC-Adresse. Wir leiten eine lokal verwaltete
  // Adresse aus der Basis-MAC des ESP32 ab, damit sie sich nicht mit dem
  // Uplink-Port beisst.
  uint8_t base_mac[6];
  esp_read_mac(base_mac, ESP_MAC_ETH);
  esp_derive_local_mac(this->mac_, base_mac);
  err = esp_eth_ioctl(this->eth_handle_, ETH_CMD_S_MAC_ADDR, this->mac_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "MAC-Adresse fuer den W5500: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool LaserProxy::init_netif_() {
  this->ip_info_.ip.addr = this->local_ip_n_;
  this->ip_info_.netmask.addr = this->subnet_n_;
  this->ip_info_.gw.addr = 0;  // kein Gateway: das Lasernetz ist eine Sackgasse

  esp_netif_inherent_config_t base = *ESP_NETIF_BASE_DEFAULT_ETH;
  base.flags = ESP_NETIF_FLAG_GARP;  // statische IP, kein DHCP-Client
  base.ip_info = &this->ip_info_;
  base.if_key = "ETH_LASER";
  base.if_desc = "laser";
  base.route_prio = 10;  // niedriger als Uplink, wird nie Default-Route
  // Keine IP_EVENT_ETH_*-Ereignisse fuer das Lasernetz: Die ESPHome-Ethernet-
  // Komponente wuerde sonst glauben, ihr Uplink habe eine IP bekommen.
  // PPP-Ereignisse wertet sie nicht aus.
  base.get_ip_event = IP_EVENT_PPP_GOT_IP;
  base.lost_ip_event = IP_EVENT_PPP_LOST_IP;

  esp_netif_config_t cfg{};
  cfg.base = &base;
  cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
  this->netif_ = esp_netif_new(&cfg);
  if (this->netif_ == nullptr) {
    ESP_LOGE(TAG, "Netzwerk-Interface fuer das Lasernetz konnte nicht angelegt werden");
    return false;
  }
  esp_err_t err = esp_netif_attach(this->netif_, esp_eth_new_netif_glue(this->eth_handle_));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_attach: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

void LaserProxy::hook_eth_events_() {
#if defined(LASER_PROXY_ISOLATE_ETH_EVENTS) && defined(USE_ETHERNET)
  esp_event_handler_t original = esphome_eth_event_handler();
  if (esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, original) == ESP_OK) {
    this->forward_handler_ = original;
    this->events_isolated_ = true;
  }
#endif
  esp_err_t err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &LaserProxy::eth_event_handler_, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ETH-Event-Handler: %s", esp_err_to_name(err));
    if (this->forward_handler_ != nullptr) {
      // Originalzustand wiederherstellen
      esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, this->forward_handler_, nullptr);
      this->forward_handler_ = nullptr;
      this->events_isolated_ = false;
    }
  }
}

void LaserProxy::eth_event_handler_(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
  auto *self = static_cast<LaserProxy *>(arg);
  esp_eth_handle_t handle = event_data != nullptr ? *static_cast<esp_eth_handle_t *>(event_data) : nullptr;
  if (handle != nullptr && handle == self->eth_handle_) {
    if (event_id == ETHERNET_EVENT_CONNECTED) {
      self->link_up_.store(true);
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED || event_id == ETHERNET_EVENT_STOP) {
      self->link_up_.store(false);
    }
    return;  // Laser-Port: nicht an ESPHome weiterreichen
  }
  if (self->forward_handler_ != nullptr)
    self->forward_handler_(nullptr, base, event_id, event_data);
}

bool LaserProxy::open_sockets_() {
  for (auto &p : this->pairs_) {
    p.sock_uplink = open_udp(htonl(INADDR_ANY), p.laser_port);
    p.sock_laser = open_udp(this->local_ip_n_, p.host_port);
    if (p.sock_uplink < 0 || p.sock_laser < 0) {
      ESP_LOGE(TAG, "UDP-Ports %u/%u lassen sich nicht oeffnen (errno %d)", p.laser_port, p.host_port, errno);
      return false;
    }
  }
  return true;
}

void LaserProxy::relay_task_(void *arg) {
  static_cast<LaserProxy *>(arg)->relay_loop_();
  vTaskDelete(nullptr);
}

void LaserProxy::relay_loop_() {
  // Im Relay-Task wird bewusst nicht geloggt: Zaehler und Zustand gehen ueber
  // Atomics an den ESPHome-Hauptloop, der sie ausgibt.
  auto *buf = new uint8_t[BUF_SIZE];
  while (true) {
    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;
    for (auto &p : this->pairs_) {
      FD_SET(p.sock_uplink, &rfds);
      FD_SET(p.sock_laser, &rfds);
      maxfd = std::max({maxfd, p.sock_uplink, p.sock_laser});
    }
    struct timeval tv {};
    tv.tv_sec = 1;
    int r = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
    if (r < 0) {
      this->dropped_++;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (r == 0)
      continue;
    for (auto &p : this->pairs_) {
      if (FD_ISSET(p.sock_uplink, &rfds))
        this->handle_from_client_(p, buf);
      if (FD_ISSET(p.sock_laser, &rfds))
        this->handle_from_laser_(p, buf);
    }
  }
}

void LaserProxy::handle_from_client_(PortPair &p, uint8_t *buf) {
  struct sockaddr_in src {};
  socklen_t slen = sizeof(src);
  int n = recvfrom(p.sock_uplink, buf, BUF_SIZE, 0, reinterpret_cast<struct sockaddr *>(&src), &slen);
  if (n <= 0)
    return;
  uint32_t sip = src.sin_addr.s_addr;
  // Pakete aus dem Lasernetz selbst gehoeren nicht auf diesen Weg.
  if ((sip & this->subnet_n_) == (this->local_ip_n_ & this->subnet_n_)) {
    this->dropped_++;
    return;
  }
  // Optionale Einschraenkung auf ein Client-Netz (Maske 0 = alle erlaubt).
  if ((sip & this->allowed_mask_n_) != (this->allowed_net_n_ & this->allowed_mask_n_)) {
    this->dropped_++;
    return;
  }

  p.client_ip = sip;
  p.client_port = src.sin_port;
  this->client_ip_.store(sip);
  this->client_port_.store(ntohs(src.sin_port));

  struct sockaddr_in dst {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(p.laser_port);
  dst.sin_addr.s_addr = this->laser_ip_n_;
  if (sendto(p.sock_laser, buf, n, 0, reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst)) < 0) {
    this->dropped_++;
  } else {
    this->to_laser_++;
  }
}

void LaserProxy::handle_from_laser_(PortPair &p, uint8_t *buf) {
  struct sockaddr_in src {};
  socklen_t slen = sizeof(src);
  int n = recvfrom(p.sock_laser, buf, BUF_SIZE, 0, reinterpret_cast<struct sockaddr *>(&src), &slen);
  if (n <= 0)
    return;
  if (src.sin_addr.s_addr != this->laser_ip_n_ || p.client_ip == 0) {
    this->dropped_++;
    return;
  }
  struct sockaddr_in dst {};
  dst.sin_family = AF_INET;
  dst.sin_addr.s_addr = p.client_ip;
  dst.sin_port = this->reply_to_host_port_ ? htons(p.host_port) : p.client_port;
  if (sendto(p.sock_uplink, buf, n, 0, reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst)) < 0) {
    this->dropped_++;
  } else {
    this->from_laser_++;
  }
}

void LaserProxy::loop() {
  bool link = this->link_up_.load();
  if (!this->link_known_ || link != this->link_published_) {
    this->link_known_ = true;
    this->link_published_ = link;
    if (link) {
      ESP_LOGI(TAG, "Lasernetz: Link aktiv");
    } else {
      ESP_LOGW(TAG, "Lasernetz: kein Link");
    }
    if (this->link_sensor_ != nullptr)
      this->link_sensor_->publish_state(link);
  }

  uint32_t ip = this->client_ip_.load();
  uint16_t port = this->client_port_.load();
  if (ip != this->client_ip_published_ || port != this->client_port_published_) {
    this->client_ip_published_ = ip;
    this->client_port_published_ = port;
    std::string text = ip == 0 ? std::string("keiner") : ip_to_string(ip) + ":" + std::to_string(port);
    if (ip != 0)
      ESP_LOGI(TAG, "Aktiver Client: %s", text.c_str());
    if (this->client_sensor_ != nullptr)
      this->client_sensor_->publish_state(text);
  }
}

void LaserProxy::update() {
  if (this->to_laser_sensor_ != nullptr)
    this->to_laser_sensor_->publish_state(this->to_laser_.load());
  if (this->from_laser_sensor_ != nullptr)
    this->from_laser_sensor_->publish_state(this->from_laser_.load());
  if (this->dropped_sensor_ != nullptr)
    this->dropped_sensor_->publish_state(this->dropped_.load());
}

void LaserProxy::dump_config() {
  ESP_LOGCONFIG(TAG, "Laser-Proxy:");
  ESP_LOGCONFIG(TAG, "  W5500: SPI%d, CLK=%d MOSI=%d MISO=%d CS=%d INT=%d RST=%d, %d Hz", this->spi_host_,
                this->clk_pin_, this->mosi_pin_, this->miso_pin_, this->cs_pin_, this->interrupt_pin_,
                this->reset_pin_, this->clock_speed_);
  ESP_LOGCONFIG(TAG, "  MAC Lasernetz: %02X:%02X:%02X:%02X:%02X:%02X", this->mac_[0], this->mac_[1], this->mac_[2],
                this->mac_[3], this->mac_[4], this->mac_[5]);
  ESP_LOGCONFIG(TAG, "  IP im Lasernetz: %s / %s", this->local_ip_.c_str(), this->subnet_.c_str());
  ESP_LOGCONFIG(TAG, "  Laser-Controller: %s", this->laser_ip_.c_str());
  for (auto &p : this->pairs_)
    ESP_LOGCONFIG(TAG, "  Kanal: Port %u -> Laser, Antworten von Port %u", p.laser_port, p.host_port);
  ESP_LOGCONFIG(TAG, "  Erlaubte Clients: %s / %s", this->allowed_net_.c_str(), this->allowed_mask_.c_str());
  ESP_LOGCONFIG(TAG, "  Antwort an festen Hostport: %s", YESNO(this->reply_to_host_port_));
  ESP_LOGCONFIG(TAG, "  Ethernet-Ereignisse vom Uplink getrennt: %s", YESNO(this->events_isolated_));
  LOG_BINARY_SENSOR("  ", "Link", this->link_sensor_);
  LOG_TEXT_SENSOR("  ", "Client", this->client_sensor_);
  LOG_SENSOR("  ", "Pakete zum Laser", this->to_laser_sensor_);
  LOG_SENSOR("  ", "Pakete vom Laser", this->from_laser_sensor_);
  LOG_SENSOR("  ", "Verworfene Pakete", this->dropped_sensor_);
  if (this->is_failed())
    ESP_LOGE(TAG, "  Einrichtung fehlgeschlagen, Details im Log beim Start");
}

}  // namespace laser_proxy
}  // namespace esphome
