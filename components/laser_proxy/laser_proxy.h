#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <atomic>
#include <string>
#include <vector>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace laser_proxy {

/// Ein weitergeleiteter UDP-Kanal des Ruida-Protokolls.
struct PortPair {
  uint16_t laser_port;      ///< Port, auf dem der Laser lauscht (z.B. 50200)
  uint16_t host_port;       ///< Port, von dem aus der Host sendet (z.B. 40200)
  int sock_uplink{-1};      ///< Vereinsnetz, gebunden an 0.0.0.0:laser_port
  int sock_laser{-1};       ///< Lasernetz, gebunden an local_ip:host_port
  uint32_t client_ip{0};    ///< letzter Absender (Netzwerk-Byte-Reihenfolge), 0 = keiner
  uint16_t client_port{0};  ///< Port des letzten Absenders (Netzwerk-Byte-Reihenfolge)
};

/// Bringt einen W5500 als eigenes Netzwerk-Interface fuer den Laser hoch und
/// leitet die UDP-Pakete zwischen Uplink-Netz und Laser weiter.
class LaserProxy : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  // Nach ethernet/wifi (setup_priority::WIFI), damit Event-Loop und esp_netif bereitstehen.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_spi_pins(int clk, int mosi, int miso, int cs) {
    this->clk_pin_ = clk;
    this->mosi_pin_ = mosi;
    this->miso_pin_ = miso;
    this->cs_pin_ = cs;
  }
  void set_interrupt_pin(int pin) { this->interrupt_pin_ = pin; }
  void set_reset_pin(int pin) { this->reset_pin_ = pin; }
  void set_clock_speed(int hz) { this->clock_speed_ = hz; }
  void set_polling_interval(uint32_t ms) { this->polling_interval_ = ms; }
  void set_spi_host(int host) { this->spi_host_ = host; }
  void set_local_ip(const std::string &ip) { this->local_ip_ = ip; }
  void set_subnet(const std::string &mask) { this->subnet_ = mask; }
  void set_laser_ip(const std::string &ip) { this->laser_ip_ = ip; }
  void set_allowed_network(const std::string &net, const std::string &mask) {
    this->allowed_net_ = net;
    this->allowed_mask_ = mask;
  }
  void add_port_pair(uint16_t laser_port, uint16_t host_port) {
    PortPair p{};
    p.laser_port = laser_port;
    p.host_port = host_port;
    this->pairs_.push_back(p);
  }
  void set_reply_to_host_port(bool v) { this->reply_to_host_port_ = v; }

  void set_link_sensor(binary_sensor::BinarySensor *s) { this->link_sensor_ = s; }
  void set_client_sensor(text_sensor::TextSensor *s) { this->client_sensor_ = s; }
  void set_to_laser_sensor(sensor::Sensor *s) { this->to_laser_sensor_ = s; }
  void set_from_laser_sensor(sensor::Sensor *s) { this->from_laser_sensor_ = s; }
  void set_dropped_sensor(sensor::Sensor *s) { this->dropped_sensor_ = s; }

 protected:
  bool init_w5500_();
  bool init_netif_();
  void hook_eth_events_();
  bool open_sockets_();
  static void eth_event_handler_(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
  static void relay_task_(void *arg);
  void relay_loop_();
  void handle_from_client_(PortPair &p, uint8_t *buf);
  void handle_from_laser_(PortPair &p, uint8_t *buf);

  // Konfiguration
  int clk_pin_{-1};
  int mosi_pin_{-1};
  int miso_pin_{-1};
  int cs_pin_{-1};
  int interrupt_pin_{-1};
  int reset_pin_{-1};
  int clock_speed_{12000000};
  uint32_t polling_interval_{10};
  int spi_host_{2};
  std::string local_ip_;
  std::string subnet_{"255.255.255.0"};
  std::string laser_ip_;
  std::string allowed_net_{"0.0.0.0"};
  std::string allowed_mask_{"0.0.0.0"};
  bool reply_to_host_port_{false};
  std::vector<PortPair> pairs_;

  // Adressen in Netzwerk-Byte-Reihenfolge (in setup() berechnet)
  uint32_t local_ip_n_{0};
  uint32_t subnet_n_{0};
  uint32_t laser_ip_n_{0};
  uint32_t allowed_net_n_{0};
  uint32_t allowed_mask_n_{0};

  // Laufzeit
  esp_eth_handle_t eth_handle_{nullptr};
  esp_netif_t *netif_{nullptr};
  esp_netif_ip_info_t ip_info_{};
  uint8_t mac_[6]{};
  esp_event_handler_t forward_handler_{nullptr};
  bool events_isolated_{false};
  TaskHandle_t task_{nullptr};

  // Vom Relay-Task bzw. Event-Handler geschrieben, im Hauptloop gelesen
  std::atomic<bool> link_up_{false};
  std::atomic<uint32_t> client_ip_{0};
  std::atomic<uint16_t> client_port_{0};
  std::atomic<uint32_t> to_laser_{0};
  std::atomic<uint32_t> from_laser_{0};
  std::atomic<uint32_t> dropped_{0};

  // Zuletzt veroeffentlichter Zustand (nur Hauptloop)
  bool link_published_{false};
  bool link_known_{false};
  uint32_t client_ip_published_{0xFFFFFFFF};
  uint16_t client_port_published_{0};

  binary_sensor::BinarySensor *link_sensor_{nullptr};
  text_sensor::TextSensor *client_sensor_{nullptr};
  sensor::Sensor *to_laser_sensor_{nullptr};
  sensor::Sensor *from_laser_sensor_{nullptr};
  sensor::Sensor *dropped_sensor_{nullptr};
};

}  // namespace laser_proxy
}  // namespace esphome
