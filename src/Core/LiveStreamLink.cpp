#include "LiveStreamLink.h"

#include <format>
#include <fstream>
#include <regex>
#include <random>
#include <chrono>

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

LiveBili::LiveBili(const std::string& roomID) :
    roomID(roomID)
{
}

LiveStreamInfo LiveBili::GetLiveStreamInfo()
{
    // 获取房间初始化信息
    auto r = cpr::Get(cpr::Url{ std::format("{}?id={}", api::live::bili::room_init.c_str(), roomID) });
    if (r.error || r.status_code != 200 || r.text.empty())
    {
        return { LiveStreamStatus::Error, "" };
    }
    try
    {
        auto roomInfo = nlohmann::json::parse(r.text, nullptr, false);
        if (roomInfo.is_discarded())
        {
            return { LiveStreamStatus::Error, "" };
        }
        int code = roomInfo["code"].get<int>();
        if (code == 60004)
        {
            return { LiveStreamStatus::Absent, "" };
        }
        if (code != 0)
        {
            return { LiveStreamStatus::Error, "" };
        }
        const auto& data = roomInfo["data"];
        int liveStatus = data["live_status"].get<int>();
        if (liveStatus != 1)
        {
            return { LiveStreamStatus::NotLive, "" };
        }
        // 更新真实房间ID
        if (data.contains("room_id"))
        {
            realRoomID = std::to_string(data["room_id"].get<int>());
        }

        std::string link = GetLinkByRealRoomID(realRoomID);
        if (link.empty())
        {
            return { LiveStreamStatus::Error, "" };
        }
        return { LiveStreamStatus::Normal, link };
    }
    catch (const nlohmann::json::exception& e)
    {
        return { LiveStreamStatus::Error, "" };
    }
}

std::string LiveBili::GetLinkByRealRoomID(const std::string& realRoomID)
{
    const cpr::Parameters params = {
#if 0
        appkey:iVGUTjsxvpLeuDCf
        build:6215200
        c_locale:zh_CN
#endif
        { "codec", "0" },
#if 0
        device:web
        device_name:VTR-AL00
        dolby:1
#endif
        { "format", "0,2" },
#if 0
        free_type:0
        http:1
        mask:0
        mobi_app:web
        network:wifi
        no_playurl:0
#endif
        { "only_audio", "0" },
        { "only_video", "0" },
#if 0
        //TODO platform 会影响下载时使用的referer
        {"platform", "h5" },
        play_type:0
#endif
        { "protocol", "0,1" },
        { "qn", "10000" },
        { "room_id", realRoomID },
#if 0
        s_locale:zh_CN
        statistics:{\"appId\":1,\"platform\":3,\"version\":\"6.21.5\",\"abtest\":\"\"}
#endif
    };
    return GetStreamUrl(params);
}

std::string LiveBili::GetStreamUrl(const cpr::Parameters param)
{
    auto r = cpr::Get(cpr::Url{ api::live::bili::v2_play_info }, param);
    if (r.error || r.status_code != 200 || r.text.empty())
    {
        return "";
    }
    try
    {
        auto playInfo = nlohmann::json::parse(r.text, nullptr, false);
        if (playInfo.is_discarded())
        {
            return "";
        }
        const auto& data = playInfo["data"];
        const auto& playurl_info = data["playurl_info"];
        const auto& playurl = playurl_info["playurl"];
        const auto& stream = playurl["stream"][0];
        const auto& format = stream["format"][0];
        const auto& codec = format["codec"][0];

        std::string base_url = codec["base_url"].get<std::string>();
        std::string extra = codec["url_info"][0]["extra"].get<std::string>();
        std::string host = codec["url_info"][0]["host"].get<std::string>();

        return host + base_url + extra;
    }
    catch (const nlohmann::json::exception& e)
    {
        return "";
    }
}

LiveDouyin::LiveDouyin(const std::string& roomID) :
    m_roomID(roomID)
{
}

LiveStreamInfo LiveDouyin::GetLiveStreamInfo()
{
    try
    {
        // 构建请求参数
        std::string user_agent =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/92.0.4515.159 Safari/537.36";

        const cpr::Header headers = {
            { "User-Agent", user_agent },
            { "referer", "https://live.douyin.com/" },
            { "cookie", []() { std::ifstream ifs{ "douyin_cookie.txt" }; std::string c; if (ifs) std::getline(ifs, c); return c; }() }
        };
        std::string params =
            "aid=6383&app_name=douyin_web&live_id=1&device_platform=web&"
            "browser_language=zh-CN&browser_platform=Win32&browser_name=Edge&"
            "browser_version=139.0.0.0&is_need_double_stream=false&web_rid=" +
            m_roomID;
        const std::string url = std::string(api::live::douyin::room) + params;
        auto response = cpr::Get(cpr::Url{ url }, headers);
        if (response.error || response.status_code != 200 || response.text.empty())
        {
            return { LiveStreamStatus::Error, "" };
        }
        auto streamInfo = nlohmann::json::parse(response.text);
        int status_code = streamInfo["status_code"].get<int>();
        if (status_code != 0)
        {
            return { LiveStreamStatus::Absent, "" };
        }
        // 获取直播数据
        const auto& data = streamInfo["data"]["data"][0];
        int status = data["status"].get<int>();
        // 抖音 status == 2 代表是开播的状态
        if (status == 2)
        {
            std::string link = GetStreamLinkFromResponse(data);
            if (link.empty())
            {
                return { LiveStreamStatus::Error, "" };
            }
            return { LiveStreamStatus::Normal, link };
        }
        // status == 4 代表未开播
        else if (status == 4)
        {
            return { LiveStreamStatus::NotLive, "" };
        }
        return { LiveStreamStatus::Error, "" };
    }
    catch (...)
    {
        return { LiveStreamStatus::Error, "" };
    }
}

std::string LiveDouyin::GetStreamLinkFromResponse(const nlohmann::json& data)
{
    try
    {
        const auto& stream_url = data["stream_url"];

        if (stream_url.contains("pull_datas"))
        {
            const auto& pullDatas = stream_url["pull_datas"];
            if (!pullDatas.empty())
            {
                auto doubleScreenStreams = pullDatas.begin().value();
                std::string stream_data_str = doubleScreenStreams["stream_data"].get<std::string>();
                auto streamData = nlohmann::json::parse(stream_data_str);

                return streamData["data"]["origin"]["main"]["flv"].get<std::string>();
            }
        }
        if (stream_url.contains("live_core_sdk_data"))
        {
            std::string stream_data_str =
                stream_url["live_core_sdk_data"]["pull_data"]["stream_data"].get<std::string>();
            auto streamData = nlohmann::json::parse(stream_data_str);

            return streamData["data"]["origin"]["main"]["flv"].get<std::string>();
        }

        return "";
    }
    catch (...)
    {
        return "";
    }
}

LiveStreamInfo GetLiveInfo(const LivePlatform platform, const std::string& roomID)
{
    switch (platform)
    {
    case LivePlatform::Douyin:
        return GetLiveInfo<LiveDouyin>(roomID);
    case LivePlatform::BiliBili:
        return GetLiveInfo<LiveBili>(roomID);
    default:
        return LiveStreamInfo{ LiveStreamStatus::Error };
    }
}
