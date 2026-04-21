#pragma once

#include "MoonrakerPrinterAgent.hpp"

#include <initializer_list>
#include <string>

namespace Slic3r {

class AnycubicPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit AnycubicPrinterAgent(std::string log_dir);
    ~AnycubicPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id) override;

protected:
    bool init_device_info(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;

private:
    bool fetch_filament_hub_info(const std::string& base_url,
                                 const std::string& api_key,
                                 std::vector<AmsTrayData>& trays,
                                 int& max_lane_index,
                                 std::string& error) const;
    bool fetch_ace_config(const std::string& base_url,
                          const std::string& api_key,
                          nlohmann::json&    ace_config,
                          std::string&       error) const;
    bool post_json(const std::string& url,
                   const std::string& api_key,
                   const std::string& payload,
                   nlohmann::json&    response,
                   std::string&       error) const;
    bool get_json(const std::string& url,
                  const std::string& api_key,
                  nlohmann::json&    response,
                  std::string&       error) const;
    static const nlohmann::json* find_json_value(const nlohmann::json& obj, std::initializer_list<const char*> keys);
    static int detect_slot_capacity(const nlohmann::json& info_json, const nlohmann::json& state_json, int highest_slot_index);
    static bool parse_filament_hub_slot(int slot_index, const nlohmann::json& filament_json, AmsTrayData& tray);
    static std::string normalize_filament_type(const std::string& filament_type);
    static std::string encode_color_rgba(const nlohmann::json& color_value);
};

} // namespace Slic3r
