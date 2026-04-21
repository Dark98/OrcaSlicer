#include "AnycubicPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/GUI_App.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>
#include <cstdio>
#include <initializer_list>

namespace Slic3r {

namespace {

constexpr const char* ANYCUBIC_AGENT_VERSION = "0.0.2";
constexpr const char* ANYCUBIC_DEFAULT_PORT = "7125";
constexpr const char* ANYCUBIC_FILAMENT_HUB_INFO_PATH = "/printer/filament_hub/info";
constexpr const char* ANYCUBIC_FILAMENT_HUB_QUERY_PATH = "/printer/objects/query?filament_hub";
constexpr const char* ANYCUBIC_FILAMENT_INFO_PATH = "/printer/filament_hub/filament_info";
constexpr const char* ANYCUBIC_ACE_CONFIG_PATH = "/server/files/config/ams_config.cfg";
constexpr int DEFAULT_ACE_SLOT_COUNT = 4;

} // anonymous namespace

AnycubicPrinterAgent::AnycubicPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo AnycubicPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"anycubic", "Anycubic", ANYCUBIC_AGENT_VERSION, "Anycubic printer agent"};
}

bool AnycubicPrinterAgent::init_device_info(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    (void) username;

    device_info         = MoonrakerDeviceInfo{};
    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) {
        return false;
    }

    auto&       preset      = preset_bundle->printers.get_edited_preset();
    const auto& printer_cfg = preset.config;
    device_info.dev_ip      = dev_ip;

    device_info.api_key    = password;
    device_info.model_name = printer_cfg.opt_string("printer_model");
    device_info.model_id   = preset.get_printer_type(preset_bundle);

    std::string configured_host = printer_cfg.opt_string("print_host");
    if (configured_host.empty()) {
        configured_host = dev_ip;
    }

    std::string configured_port = printer_cfg.opt_string("printhost_port");
    if (configured_port.empty()) {
        configured_port = ANYCUBIC_DEFAULT_PORT;
    }

    std::string base_url = normalize_base_url(configured_host, configured_port);
    if (base_url.empty()) {
        base_url = std::string(use_ssl ? "https://" : "http://") + dev_ip + ":" + ANYCUBIC_DEFAULT_PORT;
    } else if (use_ssl && boost::istarts_with(base_url, "http://")) {
        base_url.replace(0, 7, "https://");
    }

    device_info.base_url = base_url;
    device_info.dev_id   = dev_id;
    device_info.version  = "";
    device_info.dev_name = device_info.dev_id;

    return true;
}

bool AnycubicPrinterAgent::fetch_filament_info(std::string dev_id)
{
    if (device_info.base_url.empty() || device_info.dev_id != dev_id) {
        auto* dev_manager = GUI::wxGetApp().getDeviceManager();
        MachineObject* obj = nullptr;
        if (dev_manager != nullptr) {
            obj = dev_manager->get_local_machine(dev_id);
            if (obj == nullptr) {
                obj = dev_manager->get_selected_machine();
                if (obj != nullptr && obj->get_dev_id() != dev_id) {
                    obj = nullptr;
                }
            }
        }

        if (obj != nullptr) {
            init_device_info(dev_id, obj->get_dev_ip(), "bblp", obj->get_access_code(), obj->local_use_ssl);
            BOOST_LOG_TRIVIAL(info) << "AnycubicPrinterAgent::fetch_filament_info: initialized device_info for "
                                    << dev_id << " with base_url=" << device_info.base_url;
        } else {
            BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent::fetch_filament_info: unable to resolve device "
                                       << dev_id << " before sync";
        }
    }

    std::vector<AmsTrayData> trays;
    int                      max_lane_index = -1;
    std::string              error;

    if (fetch_filament_hub_info(device_info.base_url, device_info.api_key, trays, max_lane_index, error)) {
        build_ams_payload((max_lane_index + 4) / 4, max_lane_index, trays);
        return true;
    }

    BOOST_LOG_TRIVIAL(info) << "AnycubicPrinterAgent::fetch_filament_info: filament_hub API unavailable, "
                            << "falling back to ACE config: " << error;

    nlohmann::json ace_config;
    if (!fetch_ace_config(device_info.base_url, device_info.api_key, ace_config, error)) {
        BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent::fetch_filament_info: Failed to fetch ACE config: " << error;
        return false;
    }

    const auto filaments_it = ace_config.find("filaments");
    if (filaments_it == ace_config.end() || !filaments_it->is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent::fetch_filament_info: ACE config has no filament map";
        return false;
    }

    int slot_capacity = 0;
    const auto statistics_it = ace_config.find("statistics");
    if (statistics_it != ace_config.end() && statistics_it->is_object()) {
        for (const char* key : {"unwind_stat", "feed_stat"}) {
            const auto stat_it = statistics_it->find(key);
            if (stat_it != statistics_it->end() && stat_it->is_array() && stat_it->size() > 1) {
                slot_capacity = std::max(slot_capacity, static_cast<int>(stat_it->size()) - 1);
            }
        }
    }

    trays.clear();
    trays.reserve(filaments_it->size());

    int highest_slot_index = -1;
    for (const auto& [slot_key, filament] : filaments_it->items()) {
        if (!filament.is_object()) {
            continue;
        }

        int slot_index = -1;
        try {
            slot_index = std::stoi(slot_key);
        } catch (...) {
            continue;
        }

        highest_slot_index = std::max(highest_slot_index, slot_index);

        const auto type_it = filament.find("type");
        if (type_it == filament.end() || !type_it->is_string()) {
            continue;
        }

        std::string tray_type = normalize_filament_type(type_it->get<std::string>());
        if (tray_type.empty()) {
            continue;
        }

        AmsTrayData tray;
        tray.slot_index   = slot_index;
        tray.has_filament = true;
        tray.tray_type    = tray_type;
        tray.tray_color   = encode_color_rgba(filament.value("color", nlohmann::json::array()));

        auto* bundle = GUI::wxGetApp().preset_bundle;
        tray.tray_info_idx = bundle
            ? bundle->filaments.filament_id_by_type(tray.tray_type)
            : map_filament_type_to_generic_id(tray.tray_type);

        trays.emplace_back(std::move(tray));
    }

    if (slot_capacity <= 0) {
        slot_capacity = highest_slot_index >= 0 ? highest_slot_index + 1 : DEFAULT_ACE_SLOT_COUNT;
    }
    if (slot_capacity <= 0) {
        slot_capacity = DEFAULT_ACE_SLOT_COUNT;
    }

    if (trays.empty()) {
        BOOST_LOG_TRIVIAL(info) << "AnycubicPrinterAgent::fetch_filament_info: ACE config reports no loaded filaments";
        return false;
    }

    max_lane_index = std::max(slot_capacity - 1, highest_slot_index);
    build_ams_payload((max_lane_index + 4) / 4, max_lane_index, trays);
    return true;
}

bool AnycubicPrinterAgent::fetch_filament_hub_info(const std::string& base_url,
                                                   const std::string& api_key,
                                                   std::vector<AmsTrayData>& trays,
                                                   int& max_lane_index,
                                                   std::string& error) const
{
    nlohmann::json info_json;
    if (!post_json(join_url(base_url, ANYCUBIC_FILAMENT_HUB_INFO_PATH), api_key, "{}", info_json, error)) {
        return false;
    }

    nlohmann::json state_json;
    std::string    state_error;
    if (!get_json(join_url(base_url, ANYCUBIC_FILAMENT_HUB_QUERY_PATH), api_key, state_json, state_error)) {
        state_json = nlohmann::json::object();
    }

    trays.clear();
    max_lane_index      = -1;
    int highest_success = -1;

    for (int slot_index = 0; slot_index < DEFAULT_ACE_SLOT_COUNT || slot_index <= highest_success + 1; ++slot_index) {
        nlohmann::json slot_json;
        std::string    slot_error;
        const std::string payload = nlohmann::json{{"id", 0}, {"index", slot_index}}.dump();
        if (!post_json(join_url(base_url, ANYCUBIC_FILAMENT_INFO_PATH), api_key, payload, slot_json, slot_error)) {
            if (slot_index < DEFAULT_ACE_SLOT_COUNT) {
                continue;
            }
            break;
        }

        highest_success = slot_index;

        AmsTrayData tray;
        if (!parse_filament_hub_slot(slot_index, slot_json, tray)) {
            continue;
        }

        auto* bundle = GUI::wxGetApp().preset_bundle;
        tray.tray_info_idx = bundle
            ? bundle->filaments.filament_id_by_type(tray.tray_type)
            : map_filament_type_to_generic_id(tray.tray_type);

        max_lane_index = std::max(max_lane_index, slot_index);
        trays.emplace_back(std::move(tray));
    }

    if (trays.empty()) {
        error = "No filament_hub slots reported";
        return false;
    }

    const int slot_capacity = detect_slot_capacity(info_json, state_json, highest_success);
    max_lane_index          = std::max(max_lane_index, slot_capacity - 1);
    return true;
}

bool AnycubicPrinterAgent::fetch_ace_config(const std::string& base_url,
                                            const std::string& api_key,
                                            nlohmann::json&    ace_config,
                                            std::string&       error) const
{
    if (!get_json(join_url(base_url, ANYCUBIC_ACE_CONFIG_PATH), api_key, ace_config, error)) {
        return false;
    }

    if (!ace_config.is_object()) {
        error = "Invalid ACE config JSON";
        return false;
    }

    return true;
}

bool AnycubicPrinterAgent::post_json(const std::string& url,
                                     const std::string& api_key,
                                     const std::string& payload,
                                     nlohmann::json&    response,
                                     std::string&       error) const
{
    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::post(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.header("Content-Type", "application/json")
        .set_post_body(payload)
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            (void) body;
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    response = nlohmann::json::parse(response_body, nullptr, false, true);
    if (response.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    if (const auto* result = find_json_value(response, {"result"}); result != nullptr) {
        response = *result;
    }

    return true;
}

bool AnycubicPrinterAgent::get_json(const std::string& url,
                                    const std::string& api_key,
                                    nlohmann::json&    response,
                                    std::string&       error) const
{
    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            (void) body;
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    response = nlohmann::json::parse(response_body, nullptr, false, true);
    if (response.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    if (const auto* result = find_json_value(response, {"result"}); result != nullptr) {
        response = *result;
    }

    return true;
}

const nlohmann::json* AnycubicPrinterAgent::find_json_value(const nlohmann::json& obj, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        auto it = obj.find(key);
        if (it != obj.end() && !it->is_null()) {
            return &(*it);
        }
    }

    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }

        if (const auto* nested = find_json_value(*it, keys); nested != nullptr) {
            return nested;
        }
    }

    return nullptr;
}

int AnycubicPrinterAgent::detect_slot_capacity(const nlohmann::json& info_json, const nlohmann::json& state_json, int highest_slot_index)
{
    for (const auto* json_ptr : {&info_json, &state_json}) {
        if (const auto* value = find_json_value(*json_ptr, {"max_volumes", "slot_count", "slots", "max_slots"});
            value != nullptr && value->is_number_integer()) {
            const int capacity = value->get<int>();
            if (capacity > 0) {
                return capacity;
            }
        }
    }

    return highest_slot_index >= 0 ? highest_slot_index + 1 : DEFAULT_ACE_SLOT_COUNT;
}

bool AnycubicPrinterAgent::parse_filament_hub_slot(int slot_index, const nlohmann::json& filament_json, AmsTrayData& tray)
{
    if (!filament_json.is_object()) {
        return false;
    }

    tray            = AmsTrayData{};
    tray.slot_index = slot_index;

    const auto* type_value = find_json_value(filament_json, {"type", "material", "name", "filament_type"});
    if (type_value == nullptr || !type_value->is_string()) {
        return false;
    }

    tray.tray_type = normalize_filament_type(type_value->get<std::string>());
    if (tray.tray_type.empty()) {
        return false;
    }

    const auto* color_value = find_json_value(filament_json, {"color", "rgba", "color_rgba"});
    tray.tray_color         = color_value ? encode_color_rgba(*color_value) : "000000FF";

    const auto* remain_value = find_json_value(filament_json, {"remain", "remaining", "left"});
    if (remain_value != nullptr && remain_value->is_number()) {
        tray.has_filament = remain_value->get<double>() > 0.0;
    } else {
        const auto* state_value = find_json_value(filament_json, {"state", "status", "loaded"});
        if (state_value != nullptr) {
            if (state_value->is_boolean()) {
                tray.has_filament = state_value->get<bool>();
            } else if (state_value->is_number_integer()) {
                tray.has_filament = state_value->get<int>() != 0;
            } else if (state_value->is_string()) {
                const std::string state = trim_and_upper(state_value->get<std::string>());
                tray.has_filament       = state != "EMPTY" && state != "NONE" && state != "UNLOADED";
            }
        }
    }

    if (!tray.has_filament) {
        tray.has_filament = true;
    }

    const auto* bed_temp = find_json_value(filament_json, {"bed_temp", "hotbed_temp"});
    if (bed_temp != nullptr && bed_temp->is_number_integer()) {
        tray.bed_temp = bed_temp->get<int>();
    }

    const auto* nozzle_temp = find_json_value(filament_json, {"nozzle_temp", "first_layer_temp", "extruder_temp"});
    if (nozzle_temp != nullptr && nozzle_temp->is_number_integer()) {
        tray.nozzle_temp = nozzle_temp->get<int>();
    }

    return true;
}

std::string AnycubicPrinterAgent::normalize_filament_type(const std::string& filament_type)
{
    return trim_and_upper(filament_type);
}

std::string AnycubicPrinterAgent::encode_color_rgba(const nlohmann::json& color_value)
{
    if (color_value.is_string()) {
        std::string color = trim_and_upper(color_value.get<std::string>());
        if (!color.empty() && color[0] == '#') {
            color.erase(0, 1);
        }
        if (color.rfind("0X", 0) == 0) {
            color.erase(0, 2);
        }
        if (color.size() == 6) {
            color += "FF";
        }
        return color.size() == 8 ? color : "00000000";
    }

    if (!color_value.is_array()) {
        return "00000000";
    }

    int rgba[4] = {0, 0, 0, 255};
    const int count = std::min<int>(static_cast<int>(color_value.size()), 4);
    for (int i = 0; i < count; ++i) {
        if (!color_value[i].is_number_integer()) {
            return "00000000";
        }
        rgba[i] = std::clamp(color_value[i].get<int>(), 0, 255);
    }

    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X", rgba[0], rgba[1], rgba[2], rgba[3]);
    return std::string(buffer);
}

} // namespace Slic3r
