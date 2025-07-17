#include "levoit.h"
#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"
#include "esphome.h"
#include <array>

namespace esphome {
namespace levoit {

static const char *const TAG = "levoit";

void Levoit::setup() {
  ESP_LOGI(TAG, "Setting up Levoit %s", device_model_ == LevoitDeviceModel::CORE_300S ? "Core 300S" : "Core 400S");

  rx_queue_ = xQueueCreate(256, sizeof(uint8_t));
  if (rx_queue_ == NULL) {
      ESP_LOGE(TAG, "Failed to create rx queue");
      return;
  }

  tx_queue_ = xQueueCreate(8, sizeof(LevoitCommand));
  if (tx_queue_ == NULL) {
      ESP_LOGE(TAG, "Failed to create tx queue");
      return;
  }

  stateChangeMutex_ = xSemaphoreCreateMutex();
  if (stateChangeMutex_ == NULL) {
      ESP_LOGE(TAG, "Failed to create stateChangeMutex");
      return;
  }

  xTaskCreatePinnedToCore(
      [](void *param) { static_cast<Levoit *>(param)->rx_queue_task_(); },
      "RxQueueTask", 2048, this, 4, NULL, tskNO_AFFINITY);


  xTaskCreatePinnedToCore(
      [](void *param) { static_cast<Levoit *>(param)->process_tx_queue_task_(); },
      "TxQueueTask", 4096, this, 3, &procTxQueueTaskHandle_, tskNO_AFFINITY);

  xTaskCreatePinnedToCore(
      [](void *param) { static_cast<Levoit *>(param)->process_rx_queue_task_(); },
      "ProcRxQueueTask", 4096, this, 2, NULL, tskNO_AFFINITY);

  xTaskCreatePinnedToCore(
      [](void *param) { static_cast<Levoit *>(param)->maint_task_(); },
      "MaintTask", 4096, this, 1, &maintTaskHandle_, tskNO_AFFINITY);

}

void Levoit::maint_task_() {
  uint32_t lastStatusPollTime = xTaskGetTickCount();

  while (true) {
    // wait with timeout for notification
    while (ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000)) > 0) {
      if (xSemaphoreTake(stateChangeMutex_, portMAX_DELAY) == pdTRUE) {
        command_sync_();
        xSemaphoreGive(stateChangeMutex_);
      }
    }
    if (xSemaphoreTake(stateChangeMutex_, portMAX_DELAY) == pdTRUE) {
      uint32_t previousState = current_state_;

      bool wifiConnected = wifi::global_wifi_component->is_connected();
      bool haConnected = wifiConnected && esphome::api::global_api_server != nullptr && api::global_api_server->is_connected();
      bool wifiSolid = wifiConnected && haConnected;
      bool wifiFlash = wifiConnected && !haConnected;
      bool wifiOff = !wifiConnected && !haConnected;

      set_bit_(current_state_, wifiConnected, LevoitState::WIFI_CONNECTED);
      set_bit_(current_state_, haConnected, LevoitState::HA_CONNECTED);
      set_bit_(current_state_, wifiSolid, LevoitState::WIFI_LIGHT_SOLID);
      set_bit_(current_state_, wifiFlash, LevoitState::WIFI_LIGHT_FLASH);
      set_bit_(current_state_, wifiOff, LevoitState::WIFI_LIGHT_OFF);
      

      if (previousState != current_state_) {
        ESP_LOGV(TAG, "State Changed from %u to %u", previousState, current_state_);


// Wi-Fi status LED is hard-disabled on 2024+ Core 300 S FW

      }

      uint32_t removeBits = current_state_ & req_on_state_;
      if (removeBits)
        req_on_state_ &= ~removeBits;

      removeBits = ~current_state_ & req_off_state_;
      if (removeBits)
          req_off_state_ &= ~removeBits;

      xSemaphoreGive(stateChangeMutex_);
    }
    if (this->status_poll_seconds > 0 && (xTaskGetTickCount() - lastStatusPollTime) >= pdMS_TO_TICKS(this->status_poll_seconds * 1000)) {
      send_command_(LevoitCommand {
        .payloadType = LevoitPayloadType::STATUS_REQUEST,
        .packetType = LevoitPacketType::SEND_MESSAGE,
        .payload = {0x00}
      });
      lastStatusPollTime = xTaskGetTickCount();
    }
  }
}

void Levoit::command_sync_() {
  if (req_on_state_ || req_off_state_) {
    std::vector<uint8_t> command;
    bool command_sent = false;

    // Power (sniffed)
    if (req_on_state_ & static_cast<uint32_t>(LevoitState::POWER)) {
      command = {0xA5, 0x22, 0x00, 0x05, 0x00, 0x63, 0x01, 0xE0, 0xA5, 0xBC, 0x7B};
    } else if (req_off_state_ & static_cast<uint32_t>(LevoitState::POWER)) {
      command = {0xA5, 0x22, 0x01, 0x05, 0x00, 0x64, 0x00, 0xE0, 0xA5, 0xC4, 0xFD};
    }
    // Fan Speed (sniffed)
    else if (req_on_state_ & static_cast<uint32_t>(LevoitState::FAN_SPEED1)) {
      command = {0xA5, 0x22, 0x10, 0x07, 0x00, 0x73, 0x01, 0xE6, 0xA5, 0x3C, 0x05, 0xFC, 0x3F};
    } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::FAN_SPEED2)) {
      command = {0xA5, 0x22, 0x11, 0x07, 0x00, 0x74, 0x02, 0xE6, 0xA5, 0x3C, 0x05, 0xFC, 0x3F};
    } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::FAN_SPEED3)) {
      command = {0xA5, 0x22, 0x12, 0x07, 0x00, 0x75, 0x03, 0xE6, 0xA5, 0x3C, 0x05, 0xFC, 0x3F};
    }
    // Auto Mode (sniffed)
    else if (req_on_state_ & static_cast<uint32_t>(LevoitState::AUTO_DEFAULT)) {
      command = {0xA5, 0x22, 0x20, 0x07, 0x00, 0x76, 0x00, 0xE6, 0xA5, 0x3C, 0x05, 0xFC, 0x3F};
    } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::AUTO_QUIET)) {
      command = {0xA5, 0x22, 0x21, 0x07, 0x00, 0x77, 0x01, 0xE6, 0xA5, 0x3C, 0x05, 0xFC, 0x3F};
    } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::AUTO_EFFICIENT)) {
      command = {0xA5, 0x22, 0x22, 0x07, 0x00, 0x78, 0x02, 0xE6, 0xA5, 0x3C, 0x05, 0xFC, 0x3F};
    }
    // Nightlight (partially sniffed)
    else if (req_on_state_ & static_cast<uint32_t>(LevoitState::NIGHTLIGHT_OFF)) {
      command = {0xA5, 0x22, 0x40, 0x14, 0x00, 0x80, 0x01, 0x04, 0xA0, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x02, 0x00};
    }

    if (!command.empty()) {
      send_raw_command(command);
      command_sent = true;
    }

    if (!command_sent) {
      // Legacy command handling for actions without sniffed packets
      if (req_on_state_ & static_cast<uint32_t>(LevoitState::DISPLAY) || req_off_state_ & static_cast<uint32_t>(LevoitState::DISPLAY)) {
          bool commandState = req_on_state_ & static_cast<uint32_t>(LevoitState::DISPLAY);
          send_command_(LevoitCommand { .payloadType = LevoitPayloadType::SET_SCREEN_BRIGHTNESS, .packetType = LevoitPacketType::SEND_MESSAGE, .payload = {0x02, commandState ? (uint8_t)0x64 : (uint8_t)0x00} });
      } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::DISPLAY_LOCK) || req_off_state_ & static_cast<uint32_t>(LevoitState::DISPLAY_LOCK)) {
        bool commandState = req_on_state_ & static_cast<uint32_t>(LevoitState::DISPLAY_LOCK);
        send_command_(LevoitCommand { .payloadType = LevoitPayloadType::SET_DISPLAY_LOCK, .packetType = LevoitPacketType::SEND_MESSAGE, .payload = {0x01, commandState ? (uint8_t)0x01 : (uint8_t)0x00} });
      } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::FAN_SLEEP)) {
        send_command_(LevoitCommand { .payloadType = LevoitPayloadType::SET_FAN_MODE, .packetType = LevoitPacketType::SEND_MESSAGE, .payload = {0x00, 0x01} });
      } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::NIGHTLIGHT_LOW)) {
        send_command_(LevoitCommand { .payloadType = LevoitPayloadType::SET_NIGHTLIGHT, .packetType = LevoitPacketType::SEND_MESSAGE, .payload = {0x03, 0x32} });
      } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::NIGHTLIGHT_HIGH)) {
        send_command_(LevoitCommand { .payloadType = LevoitPayloadType::SET_NIGHTLIGHT, .packetType = LevoitPacketType::SEND_MESSAGE, .payload = {0x03, 0x64} });
      } else if (req_on_state_ & static_cast<uint32_t>(LevoitState::FILTER_RESET)) {
        send_command_(LevoitCommand { .payloadType = LevoitPayloadType::SET_RESET_FILTER, .packetType = LevoitPacketType::SEND_MESSAGE, .payload = {0x00, 0x00} });
      }
    }

    // Clear the request flags
    req_on_state_ = 0;
    req_off_state_ = 0;
  }
}

void Levoit::rx_queue_task_() {
  uint8_t c;
  while (true) {
    while (this->available()) {
      this->read_byte(&c);
      if (xQueueSend(rx_queue_, &c, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to send data to rx queue.");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void Levoit::process_rx_queue_task_() {
  while (true) {
    uint8_t byte;
    if (xQueueReceive(rx_queue_, &byte, pdMS_TO_TICKS(250)) == pdPASS) {
      last_rx_char_timestamp_ = millis();
      rx_message_.push_back(byte);
      // find the start of the message
      if (rx_message_.size() == 1) {
        if (rx_message_[0] != 0xA5)
          rx_message_.clear();
      } else if (rx_message_.size() == 2) {
        if(rx_message_[1] != 0x12 && rx_message_[1] != 0x52)
          rx_message_.clear();
      } else if (rx_message_.size() > 4) {
        uint8_t msg_len = rx_message_[3];
        if (rx_message_.size() == msg_len + 5) { // header + len + type + payload + checksum
          ESP_LOGD("levoit-uart", "RX: %s", format_hex_pretty(rx_message_.data(), rx_message_.size()).c_str());
          if(validate_message_()) {
            uint8_t msg_type = rx_message_[2];
            uint32_t payloadType = (rx_message_[5] << 16) + (rx_message_[6] << 8) + rx_message_[7];
            handle_payload_(static_cast<LevoitPayloadType>(payloadType), &rx_message_[4], msg_len);
          }
          rx_message_.clear();
        }
      }
    } else {
      if (rx_message_.size() > 0 && (millis() - last_rx_char_timestamp_) > 250) {
        ESP_LOGD(TAG, "RX Message Timeout");
        rx_message_.clear();
      }
    }
  }
}

void Levoit::dump_config() { 
    ESP_LOGCONFIG(TAG, "Levoit!"); 
    ESP_LOGCONFIG(TAG, "  Command Delay: %d ms", this->command_delay_);
    ESP_LOGCONFIG(TAG, "  Command Timeout: %d ms", this->command_timeout_);
    ESP_LOGCONFIG(TAG, "  Status Poll Seconds: %d s", this->status_poll_seconds);
}

bool Levoit::validate_message_() {
  uint32_t at = this->rx_message_.size() - 1;
  auto *data = &this->rx_message_[0];
  uint8_t new_byte = data[at];

  if (at == 0)
    return new_byte == 0xA5;

  if (at == 1) {
    if (new_byte == 0x52) {
      ESP_LOGE(TAG, "Received error response, ignoring packet");
      if (xSemaphoreTake(stateChangeMutex_, portMAX_DELAY) == pdTRUE) {
        req_off_state_ = 0;
        req_on_state_ = 0;
        xSemaphoreGive(stateChangeMutex_);
      }
      return false;
    }
    return (new_byte == 0x12) || (new_byte == 0x22);
  }

  uint8_t sequenceNumber = data[2];
  if (at == 2)
    return true;

  uint8_t payloadLength = data[3];
  if (at == 3) {
    return true;
  }

  if (at == 4)
    return (new_byte == 0x00);

  uint8_t payloadChecksum = data[5];
  if (at == 5) {
    return true;
  }

  if (at - 5 < payloadLength) {
    return true;
  }

  uint8_t calc_checksum = 255;
  for (uint8_t i = 0; i < 6 + payloadLength; i++) {
    if (i != 5) {
      calc_checksum -= data[i];
    }
  }

  if (payloadChecksum != calc_checksum) {
    ESP_LOGE(TAG, "Received invalid message checksum, ignoring packet");
    return false;
  }

  // valid message
  const uint8_t *message_data = data + 6;

  LevoitPayloadType payloadType = 
    (LevoitPayloadType) (message_data[2] | (message_data[1] << 8) | (message_data[0] << 16) | (0x00 << 24));

  uint8_t *payload_data = data + 9;

  // If it's not a 1-byte ACK response, handle the payload.
  if (data[1] != 0x12 || payloadLength - 3 != 1) {
    this->handle_payload_(payloadType, payload_data, payloadLength - 3);
  } else if (data[1] == 0x12) {
    ESP_LOGV(TAG, "Received ACK (%06x)", (uint32_t) payloadType);
  }

  // acknowledge packet if required
  if (data[1] == 0x22) {
    LevoitCommand acknowledgeResponse = {
        .payloadType = payloadType, .packetType = LevoitPacketType::ACK_MESSAGE, .payload = {0x00}};
    this->send_raw_command(acknowledgeResponse);
  } else if (data[1] == 0x12) {
    // notify wait for ACK
    xTaskNotifyGive(procTxQueueTaskHandle_);
  }

  this->sequenceNumber_ = sequenceNumber + 1;

  // return false to reset rx buffer
  return false;
}

void Levoit::handle_payload_(LevoitPayloadType type, uint8_t *payload, size_t len) {
  uint32_t previousState = current_state_;

  switch (type) {
    case LevoitPayloadType::STATUS_RESPONSE:
      set_bit_(current_state_, payload[2] == 0x01, LevoitState::POWER);
      set_bit_(current_state_, payload[5] == 0x01, LevoitState::DISPLAY_LOCK);
      set_bit_(current_state_, payload[8] > 0, LevoitState::DISPLAY);
      set_bit_(current_state_, payload[3] == 0x00, LevoitState::FAN_MANUAL);
      set_bit_(current_state_, payload[3] == 0x01, LevoitState::FAN_SLEEP);
      set_bit_(current_state_, payload[3] == 0x02, LevoitState::FAN_AUTO);

      if ((current_state_ & static_cast<uint32_t>(LevoitState::FAN_MANUAL)) || (current_state_ & static_cast<uint32_t>(LevoitState::FAN_SLEEP))) {
        set_bit_(current_state_, payload[4] == 0x01, LevoitState::FAN_SPEED1);
        set_bit_(current_state_, payload[4] == 0x02, LevoitState::FAN_SPEED2);
        set_bit_(current_state_, payload[4] == 0x03, LevoitState::FAN_SPEED3);
        set_bit_(current_state_, payload[4] == 0x04, LevoitState::FAN_SPEED4);
      }

      if (payload[1] != pm25_value) {
        set_bit_(current_state_, true, LevoitState::PM25_CHANGE);
        pm25_value = payload[1];
      }

      if (payload[19] != air_quality) {
        set_bit_(current_state_, true, LevoitState::AIR_QUALITY_CHANGE);
        air_quality = payload[19];
      }

      break;
    
    case LevoitPayloadType::AUTO_STATUS:
        set_bit_(current_state_, payload[1] == 0x00, LevoitState::AUTO_DEFAULT);
        set_bit_(current_state_, payload[1] == 0x01, LevoitState::AUTO_QUIET);
        set_bit_(current_state_, payload[1] == 0x02, LevoitState::AUTO_EFFICIENT);
        break;

    default:
      ESP_LOGW(TAG, "Unhandled payload type: %08X", type);
      ESP_LOGW(TAG, "Payload: %s", format_hex_pretty(payload, len).c_str());
      break;
  }

  if (previousState != current_state_) {
    for (auto &listener : state_listeners_) {
      if (previousState & listener.mask != current_state_ & listener.mask)
        listener.func(current_state_);
    }
  }
}

void Levoit::set_request_state(uint32_t onMask, uint32_t offMask, bool aquireMutex) {
    if (aquireMutex && xSemaphoreTake(stateChangeMutex_, portMAX_DELAY) == pdTRUE) {
      if (onMask & offMask) {
        ESP_LOGE(TAG, "set_request_state - tried to set same bit on and off");
        return;
      }

      // Filter out bits in onMask that are already on in current_state_
      onMask &= ~current_state_;

      // Filter out bits in offMask that are already off in current_state_
      offMask &= current_state_;

      if (onMask > 0) {
        req_on_state_ |= onMask;
        req_off_state_ &= ~onMask;
      }

      if (offMask > 0) {
        req_off_state_ |= offMask;
        req_on_state_ &= ~offMask;
      }

      ESP_LOGV(TAG, "set_request_state - Current State: %u, Requested On: %u, Request Off: %u", current_state_, req_on_state_, req_off_state_);

      if (aquireMutex)
        xSemaphoreGive(stateChangeMutex_);

      xTaskNotifyGive(maintTaskHandle_);
    }
}

void Levoit::set_bit_(uint32_t &state, bool condition, LevoitState bit) {
    if (condition)
        state |= static_cast<uint32_t>(bit);
    else
        state &= ~static_cast<uint32_t>(bit);
}

void Levoit::register_state_listener(uint32_t changeMask, const std::function<void(uint32_t currentBits)> &func) {
  auto listener = LevoitStateListener{
      .mask = changeMask,
      .func = func,
  };
  this->state_listeners_.push_back(listener);
}

void Levoit::process_tx_queue_task_() {
  LevoitCommand command;

  while (true) {
    if (xQueueReceive(tx_queue_, &command, portMAX_DELAY) == pdPASS) {
      process_raw_command_(command);
    }
  }
}

void Levoit::send_raw_command(LevoitCommand command) {
  if (xQueueSend(tx_queue_, &command, pdMS_TO_TICKS(10)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to send data to tx queue.");
  }
}

void Levoit::send_raw_command(std::vector<uint8_t> &command) {
  uint8_t tx_len = command.size();

  if (tx_len > 0) {
    if(this->command_delay_ > 0) {
      if (last_command_timestamp_ > 0 && millis() - last_command_timestamp_ < this->command_delay_) {
        delay(this->command_delay_ - (millis() - last_command_timestamp_));
      }
    }
    
    this->write_array(command.data(), tx_len);
    this->flush();
    ESP_LOGD("levoit-uart", "TX: %s", format_hex_pretty(command.data(), command.size()).c_str());
    last_command_timestamp_ = millis();
  }
}

void Levoit::process_raw_command_(LevoitCommand command) {
  this->last_command_timestamp_ = millis();

  sequenceNumber_++;

  uint8_t payloadTypeByte1 = ((uint32_t) command.payloadType >> 16) & 0xff;
  uint8_t payloadTypeByte2 = ((uint32_t) command.payloadType >> 8) & 0xff;
  uint8_t payloadTypeByte3 = (uint32_t) command.payloadType & 0xff;

  // Initialize the outgoing packet
  std::vector<uint8_t> rawPacket = {
    0xA5,
    (uint8_t) command.packetType,
    sequenceNumber_,
    (uint8_t) (command.payload.size() + 3),
    0x00,
    0x00,
    payloadTypeByte1,
    payloadTypeByte2,
    payloadTypeByte3
  };

  if (!command.payload.empty())
    rawPacket.insert(rawPacket.end(), command.payload.begin(), command.payload.end());

  // Calculate checksum & insert into packet
  uint8_t checksum = 255;
  for (uint8_t i = 0; i < rawPacket.size(); i++) {
    if (i != 5) {
      checksum -= rawPacket[i];
    }
  }
  rawPacket[5] = checksum;

  const char *packetTypeStr = (command.packetType == LevoitPacketType::ACK_MESSAGE)
                                  ? "ACK"
                                  : ((command.packetType == LevoitPacketType::SEND_MESSAGE) ? "CMD" : "UNKNOWN");
  this->write_array(rawPacket);
  ESP_LOGV(TAG, "Sending %s (%06x): %s", packetTypeStr, (uint32_t) command.payloadType,
           format_hex_pretty(rawPacket.data(), rawPacket.size()).c_str());

  // await ACK, not really needed but helps with pacing commands
  // command will retry if state is not achieved
  if (command.packetType != LevoitPacketType::ACK_MESSAGE && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(command_timeout_)) == 0) {
    ESP_LOGW(TAG, "Timeout waiting for ACK for command %s (%06x): %s", packetTypeStr, (uint32_t) command.payloadType,
          format_hex_pretty(rawPacket.data(), rawPacket.size()).c_str());
  }
}

void Levoit::send_command_(const LevoitCommand &command) {
  auto modified_command = command;
  modified_command.payloadType = static_cast<LevoitPayloadType>(get_model_specific_payload_type(command.payloadType));
  if (xQueueSend(tx_queue_, &modified_command, pdMS_TO_TICKS(10)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to send data to tx queue.");
  }
}


void Levoit::set_command_delay(int delay) {
  command_delay_ = delay;
}

void Levoit::set_command_timeout(int timeout) {
  command_timeout_ = timeout;
}

void Levoit::set_status_poll_seconds(int interval) {
  status_poll_seconds = interval;
}

void Levoit::set_device_model(std::string model) {
  if (model == "core300s") {
    device_model_ = LevoitDeviceModel::CORE_300S;
  } else if (model == "core400s") {
    device_model_ = LevoitDeviceModel::CORE_400S;
  } else if (model == "core200s") {
    device_model_ = LevoitDeviceModel::CORE_200S;
  } else {
    ESP_LOGW(TAG, "Unknown device model: %s", model.c_str());
  }
}

uint32_t Levoit::get_model_specific_payload_type(LevoitPayloadType type) {
  auto model_itr = MODEL_SPECIFIC_PAYLOAD_TYPES.find(device_model_);
  if (model_itr != MODEL_SPECIFIC_PAYLOAD_TYPES.end()) {
    auto payload_itr = model_itr->second.find(type);
    if (payload_itr != model_itr->second.end()) {
      return payload_itr->second;
    }
  }
  // If no override is found, return the default payload
  return static_cast<uint32_t>(type);
}

}  // namespace levoit
}  // namespace esphome
