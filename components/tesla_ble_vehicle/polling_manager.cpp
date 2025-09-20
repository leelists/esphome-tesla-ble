#include "polling_manager.h"
#include "tesla_ble_vehicle.h"
#include "common.h"
#include <client.h>
#include "log.h"
#include <vector>

namespace esphome {
namespace tesla_ble_vehicle {

PollingManager::PollingManager(TeslaBLEVehicle* parent)
    : parent_(parent), last_vcsec_poll_(0), last_infotainment_poll_(0),
      connection_time_(0), just_connected_(false), was_awake_(false),
      was_charging_(false), was_unlocked_(false), was_user_present_(false) {}

void PollingManager::update() {
    if (!parent_->is_connected()) {
        ESP_LOGV(POLLING_MANAGER_TAG, "Not connected, skipping polling");
        return;
    }

    ESP_LOGD(POLLING_MANAGER_TAG, "Polling manager update called (just_connected=%s)", just_connected_ ? "true" : "false");
    
    uint32_t now = millis();
    
    // Handle initial connection - start with VCSEC only, defer infotainment poll
    if (just_connected_) {
        ESP_LOGI(POLLING_MANAGER_TAG, "Just connected - performing initial VCSEC poll, deferring infotainment poll");
        request_vcsec_poll();
        last_vcsec_poll_ = now;
        pending_initial_infotainment_poll_ = true;
        just_connected_ = false;
        return;
    }

    // Always poll VCSEC since ESPHome update_interval matches vcsec_poll_interval
    log_polling_decision("VCSEC status poll", "Regular interval (ESPHome update)");
    request_vcsec_poll();
    last_vcsec_poll_ = now;

    // Check if we should poll infotainment based on smart polling logic
    bool should_poll_infotainment_now = should_poll_infotainment();

    ESP_LOGV(POLLING_MANAGER_TAG, "Infotainment polling check: %s", 
             should_poll_infotainment_now ? "yes" : "no");

    if (should_poll_infotainment_now) {
        std::string reason = get_fast_poll_reason();
        log_polling_decision("Infotainment data poll", reason);
        request_infotainment_poll();
        last_infotainment_poll_ = now;
    }
}

void PollingManager::handle_connection_established() {
    ESP_LOGI(POLLING_MANAGER_TAG, "BLE connection established - setting just_connected flag");
    
    uint32_t now = millis();
    connection_time_ = now;
    wake_time_ = now;  // Assume vehicle is waking up when we connect
    just_connected_ = true;
    
    reset_state_cache();
    reset_polling_timestamps();
    
    ESP_LOGD(POLLING_MANAGER_TAG, "Connection setup complete - ready for immediate poll");
}

void PollingManager::handle_connection_lost() {
    ESP_LOGI(POLLING_MANAGER_TAG, "BLE connection lost");
    
    just_connected_ = false;
    connection_time_ = 0;
    
    reset_state_cache();
    reset_polling_timestamps();
}

void PollingManager::update_vehicle_state(bool is_awake, bool is_charging, bool is_unlocked, bool is_user_present) {
    bool state_changed = (was_awake_ != is_awake) || 
                        (was_charging_ != is_charging) || 
                        (was_unlocked_ != is_unlocked) ||
                        (was_user_present_ != is_user_present);
    
    if (state_changed) {
        ESP_LOGD(POLLING_MANAGER_TAG, "Vehicle state changed: awake=%s, charging=%s, unlocked=%s, user_present=%s",
                 is_awake ? "true" : "false",
                 is_charging ? "true" : "false", 
                 is_unlocked ? "true" : "false",
                 is_user_present ? "true" : "false");
        
        // If vehicle just woke up, track wake time and poll infotainment immediately
        if (!was_awake_ && is_awake) {
            wake_time_ = millis();
            ESP_LOGI(POLLING_MANAGER_TAG, "Vehicle just woke up - tracking wake time and requesting immediate infotainment poll");
            request_infotainment_poll(true);  // true = bypass delay
            last_infotainment_poll_ = millis();
            // Cancel pending initial infotainment poll since we're doing it now due to wake
            pending_initial_infotainment_poll_ = false;
        }
    }
    
    was_awake_ = is_awake;
    was_charging_ = is_charging;
    was_unlocked_ = is_unlocked;
    was_user_present_ = is_user_present;
}

void PollingManager::force_immediate_poll() {
    ESP_LOGI(POLLING_MANAGER_TAG, "Force immediate poll requested");
    
    uint32_t now = millis();
    
    // Force VCSEC poll
    request_vcsec_poll();
    last_vcsec_poll_ = now;
    
    // Force infotainment poll if awake
    if (was_awake_) {
        request_infotainment_poll();
        last_infotainment_poll_ = now;
    }
}

bool PollingManager::should_poll_infotainment() {
    uint32_t now = millis();
    
    // Give a grace period after connection to let VCSEC establish vehicle state first
    uint32_t time_since_connection = time_since(connection_time_);
    if (connection_time_ > 0 && time_since_connection < CONNECTION_GRACE_PERIOD) {
        ESP_LOGV(POLLING_MANAGER_TAG, "Within connection grace period (%u ms), skipping infotainment poll", time_since_connection);
        return false;
    }
    
    // Don't poll infotainment if vehicle is asleep
    if (!was_awake_) {
        ESP_LOGV(POLLING_MANAGER_TAG, "Vehicle asleep, skipping infotainment poll");
        return false;
    }

    // If charging, always poll at active interval regardless of wake time
    if (was_charging_) {
        if (has_elapsed(last_infotainment_poll_, infotainment_poll_interval_active_)) {
            ESP_LOGV(POLLING_MANAGER_TAG, "Vehicle charging, polling at active interval");
            wake_time_ = millis(); //Reset the wake time to keep infotainment polling going for a while after end charge
            return true;
        }
        return false;
    }

    // If not charging, check if we're within the wake window
    uint32_t time_since_wake = time_since(wake_time_);
    if (time_since_wake >= infotainment_sleep_timeout_) {
        ESP_LOGV(POLLING_MANAGER_TAG, "Vehicle awake for %u ms (>%u ms), allowing sleep - skipping infotainment poll", 
                 time_since_wake, infotainment_sleep_timeout_);
        return false;
    }
 
    // We're within the wake window, poll at awake interval
    if (has_elapsed(last_infotainment_poll_, infotainment_poll_interval_awake_)) {
        ESP_LOGV(POLLING_MANAGER_TAG, "Vehicle awake for %u ms (<%u ms), polling at awake interval", 
                 time_since_wake, infotainment_sleep_timeout_);
        return true;
    }
    
    return false;
}

uint32_t PollingManager::get_infotainment_poll_interval() {
    if (should_use_fast_polling()) {
        return infotainment_poll_interval_active_;
    } else {
        return infotainment_poll_interval_awake_;
    }
}

bool PollingManager::should_use_fast_polling() {
    // Fast poll if charging, unlocked, or user is present (user might be actively using the vehicle)
    return was_charging_ || was_unlocked_ || was_user_present_;
}

std::string PollingManager::get_fast_poll_reason() {
    std::vector<std::string> reasons;
    if (was_charging_) reasons.push_back("charging");
    if (was_unlocked_) reasons.push_back("unlocked");
    if (was_user_present_) reasons.push_back("user present");
    
    if (reasons.empty()) {
        return "vehicle awake";
    } else if (reasons.size() == 1) {
        return reasons[0];
    } else {
        std::string result = reasons[0];
        for (size_t i = 1; i < reasons.size(); ++i) {
            if (i == reasons.size() - 1) {
                result += " and " + reasons[i];
            } else {
                result += ", " + reasons[i];
            }
        }
        return result;
    }
}

void PollingManager::request_vcsec_poll() {
    ESP_LOGD(POLLING_MANAGER_TAG, "Requesting VCSEC poll");
    
    auto* command_manager = parent_->get_command_manager();
    if (!command_manager) {
        ESP_LOGE(POLLING_MANAGER_TAG, "Command manager not available");
        return;
    }
    
    command_manager->enqueue_vcsec_poll();
}

void PollingManager::request_infotainment_poll(bool bypass_delay) {
    ESP_LOGD(POLLING_MANAGER_TAG, "Requesting infotainment poll (bypass_delay=%s)", bypass_delay ? "true" : "false");
    
    // Check if we should delay this request due to recent commands
    if (!bypass_delay) {
        uint32_t now = millis();
        uint32_t time_since_last = Utils::time_since(now, last_infotainment_poll_);
        constexpr uint32_t MIN_COMMAND_INTERVAL = 2000; // 2 seconds minimum between commands
        if (time_since_last < MIN_COMMAND_INTERVAL) {
            ESP_LOGV(POLLING_MANAGER_TAG, "Skipping infotainment poll - too soon after last poll (%u ms ago)", time_since_last);
            return;
        }
    }
    
    auto* command_manager = parent_->get_command_manager();
    if (!command_manager) {
        ESP_LOGE(POLLING_MANAGER_TAG, "Command manager not available");
        return;
    }
    
    command_manager->enqueue_infotainment_poll();
}

void PollingManager::request_wake_and_poll() {
    ESP_LOGI(POLLING_MANAGER_TAG, "Wake and poll requested");
    
    auto* command_manager = parent_->get_command_manager();
    if (!command_manager) {
        ESP_LOGE(POLLING_MANAGER_TAG, "Command manager not available");
        return;
    }
    
    // Wake command
    command_manager->enqueue_wake_vehicle();
    
    // Then request VCSEC poll to get updated status
    command_manager->enqueue_vcsec_poll();
}

void PollingManager::force_infotainment_poll() {
    ESP_LOGI(POLLING_MANAGER_TAG, "Force infotainment poll requested (bypassing delay)");
    auto* command_manager = parent_->get_command_manager();
    if (command_manager) {
        command_manager->enqueue_infotainment_poll();
    }
}

void PollingManager::force_full_update() {
    ESP_LOGI(POLLING_MANAGER_TAG, "Force full update requested (no wake command)");
    
    auto* command_manager = parent_->get_command_manager();
    if (!command_manager) {
        return;
    }
    
    // Request fresh VCSEC data first (to get current sleep/awake status)
    command_manager->enqueue_vcsec_poll();
    
    // Then request infotainment data (bypassing any delays)
    command_manager->enqueue_infotainment_poll();
}

uint32_t PollingManager::time_since_last_vcsec_poll() const {
    if (last_vcsec_poll_ == 0) {
        return UINT32_MAX;
    }
    return time_since(last_vcsec_poll_);
}

uint32_t PollingManager::time_since_last_infotainment_poll() const {
    if (last_infotainment_poll_ == 0) {
        return UINT32_MAX;
    }
    return time_since(last_infotainment_poll_);
}

// Rollover-safe time calculations
uint32_t PollingManager::time_since(uint32_t timestamp) const {
    return Utils::time_since(millis(), timestamp);
}

bool PollingManager::has_elapsed(uint32_t timestamp, uint32_t interval) const {
    return time_since(timestamp) >= interval;
}

void PollingManager::log_polling_decision(const std::string& action, const std::string& reason) {
    ESP_LOGD(POLLING_MANAGER_TAG, "Polling decision: %s (reason: %s)", action.c_str(), reason.c_str());
}

void PollingManager::reset_state_cache() {
    was_awake_ = false;
    was_charging_ = false;
    was_unlocked_ = false;
    was_user_present_ = false;
}

void PollingManager::reset_polling_timestamps() {
    last_vcsec_poll_ = 0;
    last_infotainment_poll_ = 0;
}

void PollingManager::handle_initial_vcsec_poll_complete() {
    if (pending_initial_infotainment_poll_) {
        ESP_LOGI(POLLING_MANAGER_TAG, "Initial VCSEC poll complete - requesting initial infotainment poll");
        auto* command_manager = parent_->get_command_manager();
        if (command_manager) {
            command_manager->enqueue_infotainment_poll();
        }
        last_infotainment_poll_ = millis();
        pending_initial_infotainment_poll_ = false;
    }
}

} // namespace tesla_ble_vehicle
} // namespace esphome
