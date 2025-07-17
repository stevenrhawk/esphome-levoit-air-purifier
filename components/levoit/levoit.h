#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <freertos/semphr.h>
#include <vector>
#include <unordered_map>

namespace esphome {
namespace levoit {

enum class LevoitDeviceModel : uint8_t { NONE, CORE_200S, CORE_300S, CORE_400S };
enum class LevoitPacketType : uint8_t { SEND_MESSAGE = 0x22, ACK_MESSAGE = 0x12, ERROR = 0x52 };
enum class LevoitPayloadType : uint32_t {
  /* ---------- Core‑300 S (FW v2, 2024+) ---------- */
  STATUS_REQUEST        = 0x013240,
  STATUS_RESPONSE       = 0x013140,
  AUTO_STATUS           = 0x016140,   // confirmed on your trace

  /* Fan control ---------------------------------------------------------- */
  SET_FAN_AUTO_MODE     = 0x01E7A5,   // auto‑mode (Default / Quiet / Efficient)
  SET_FAN_MANUAL        = 0x0161A2,   // manual RPM (payload[3] = 1‑4)
  SET_FAN_MODE          = 0x01E1A5,   // switch Manual / Sleep / Auto

  /* Toggles & settings ----------------------------------------------------*/
  SET_POWER_STATE       = 0x0100A6,   // 0 = off, 1 = on
  SET_DISPLAY_LOCK      = 0x0101D1,   // payload[15] = 0/1
  SET_SCREEN_BRIGHTNESS = 0x0106A1,   // payload[8]  = 0 / 0x64
  SET_NIGHTLIGHT        = 0x0104A0,   // payload[8]  = 0 / 0x32 / 0x64
  SET_WIFI_STATUS_LED   = 0x012AA1,

  /* Misc ------------------------------------------------------------------*/
  SET_FILTER_LED        = 0x01E3A5,
  SET_RESET_FILTER      = 0x01E5A5,
  TIMER_STATUS          = 0x0166A2,
  SET_TIMER_TIME        = 0x0165A2,
  TIMER_START_OR_CLEAR  = 0x0167A2
};

enum class LevoitState : uint32_t {
  POWER = 1,
  FAN_MANUAL = 2,
  FAN_AUTO = 4,
  FAN_SLEEP = 8,
  DISPLAY = 16,
  DISPLAY_LOCK = 32,
  FAN_SPEED1 = 64,
  FAN_SPEED2 = 128,
  FAN_SPEED3 = 256,
  FAN_SPEED4 = 512,
  NIGHTLIGHT_OFF = 1024,
  NIGHTLIGHT_LOW = 2048,
  NIGHTLIGHT_HIGH = 4096,
  AUTO_DEFAULT = 8192,
  AUTO_QUIET = 16384,
  AUTO_EFFICIENT = 32768,
  AIR_QUALITY_CHANGE = 65536,
  PM25_NAN = 131072,
  PM25_CHANGE = 262144,
  WIFI_CONNECTED = 524288,
  HA_CONNECTED = 1048576,
  FILTER_RESET = 2097152,
  WIFI_LIGHT_SOLID = 4194304,
  WIFI_LIGHT_FLASH = 8388608,
  WIFI_LIGHT_OFF = 16777216
};

struct LevoitStateListener {
  uint32_t mask;
  std::function<void(uint32_t currentBits)> func;
};

typedef struct LevoitCommand {
  LevoitPayloadType payloadType;
  LevoitPacketType packetType;
  std::vector<uint8_t> payload;
} LevoitPacket;

using PayloadTypeOverrideMap = std::unordered_map<LevoitDeviceModel, std::unordered_map<LevoitPayloadType, uint32_t>>;

static const PayloadTypeOverrideMap MODEL_SPECIFIC_PAYLOAD_TYPES = {
    {LevoitDeviceModel::CORE_400S, {
        {LevoitPayloadType::STATUS_REQUEST,  0x01B140},
        {LevoitPayloadType::STATUS_RESPONSE, 0x01B040},
    }},
<<<<<<< HEAD
=======
    {LevoitDeviceModel::CORE_300S, {
        {LevoitPayloadType::STATUS_REQUEST,        0x013240},
        {LevoitPayloadType::STATUS_RESPONSE,       0x013140},
        {LevoitPayloadType::AUTO_STATUS,           0x016140},
    
        {LevoitPayloadType::SET_FAN_AUTO_MODE,     0x01E7A5},   // ← sniffed value
        {LevoitPayloadType::SET_FAN_MANUAL,        0x0161A2},   // ← sniffed value
        {LevoitPayloadType::SET_FAN_MODE,          0x01E1A5},   // ← sniffed value
    
        {LevoitPayloadType::SET_POWER_STATE,       0x0100A6},   // ← sniffed value
        {LevoitPayloadType::SET_DISPLAY_LOCK,      0x0101D1},   // ← sniffed value
        {LevoitPayloadType::SET_SCREEN_BRIGHTNESS, 0x0106A1},   // ← sniffed value
        {LevoitPayloadType::SET_NIGHTLIGHT,        0x0104A0},   // ← sniffed value
        {LevoitPayloadType::SET_WIFI_STATUS_LED,   0x012AA1},
        {LevoitPayloadType::SET_FILTER_LED,        0x01E3A5},
        {LevoitPayloadType::SET_RESET_FILTER,      0x01E5A5},
        {LevoitPayloadType::TIMER_STATUS,          0x0166A2},
        {LevoitPayloadType::SET_TIMER_TIME,        0x0165A2},
        {LevoitPayloadType::TIMER_START_OR_CLEAR,  0x0167A2}
    }},

>>>>>>> a04ddee39b979fc22ac6a17cea4b614958f6f0bd
    {LevoitDeviceModel::CORE_200S, {
        {LevoitPayloadType::STATUS_REQUEST,  0x016140},
        {LevoitPayloadType::STATUS_RESPONSE, 0x016040},
        {LevoitPayloadType::AUTO_STATUS,     0x016040},
    }},
};


class Levoit : public Component, public uart::UARTDevice {
 public:
  LevoitDeviceModel device_model_ = LevoitDeviceModel::CORE_300S;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void dump_config() override;
  void set_device_model(std::string model);
  void set_command_delay(int delay);
  void set_command_timeout(int timeout);
  void set_status_poll_seconds(int interval);
  void register_state_listener(uint32_t changeMask, const std::function<void(uint32_t currentBits)> &func);
  void set_request_state(uint32_t onMask, uint32_t offMask, bool aquireMutex = true);
  uint32_t get_model_specific_payload_type(LevoitPayloadType type);
  uint32_t fanChangeMask =
    static_cast<uint32_t>(LevoitState::FAN_SPEED1) +
    static_cast<uint32_t>(LevoitState::FAN_SPEED2) +
    static_cast<uint32_t>(LevoitState::FAN_SPEED3) +
    static_cast<uint32_t>(LevoitState::FAN_SPEED4) +
    static_cast<uint32_t>(LevoitState::FAN_SLEEP);
  uint32_t pm25_value = 1000;
  uint8_t air_quality = 255;

 protected:
  QueueHandle_t rx_queue_;
  QueueHandle_t tx_queue_;
  SemaphoreHandle_t stateChangeMutex_;
  TaskHandle_t procTxQueueTaskHandle_;
  TaskHandle_t maintTaskHandle_;
  uint32_t current_state_ = 0;
  uint32_t req_on_state_ = 0;
  uint32_t req_off_state_ = 0;
  uint32_t command_delay_;
  uint32_t command_timeout_;
  uint32_t last_command_timestamp_ = 0;
  uint32_t last_rx_char_timestamp_ = 0;
  uint32_t status_poll_seconds;
  uint8_t sequenceNumber_ = 0;
  std::vector<uint8_t> rx_message_;
  std::vector<LevoitStateListener> state_listeners_;
  void rx_queue_task_();
  void process_rx_queue_task_();
  void process_tx_queue_task_();
  void maint_task_();
  void command_sync_();
  void send_command_(const LevoitCommand &command);
  /** Build & send the 20-byte “extended” payloads used by lock /
   *  display brightness / night-light on the Core 300 S (2024+ FW) */
  void send_long_payload_(LevoitPayloadType pt,
                          uint8_t purpose_code,
                          uint8_t byte15_value);
  void process_raw_command_(LevoitCommand command);
  void send_raw_command(std::vector<uint8_t> &command);
  void set_bit_(uint32_t &state, bool condition, LevoitState bit);
  bool validate_message_();
  void handle_payload_(LevoitPayloadType type, uint8_t *payload, size_t len);
  
};

}  // namespace levoit
}  // namespace esphome
