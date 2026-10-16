#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "ApiDefs.hpp"
#include "UrlQuery.hpp"

class ScannerBase
{
public:
    GameType gameType;
    std::string_view scanUrl{};
    std::string_view confirmUrl{};
    std::string lastTicket;
    std::string lastQrCode;
    std::string uid;
    std::string gameToken{};
    std::string mid;
    std::map<std::string_view, std::function<void()>> setGameTypeByBizKey{
        { "bh3_cn", [this]() {
             gameType = GameType::Honkai3;
             scanUrl = api::mhy::bh3::qrcode_scan;
             confirmUrl = api::mhy::bh3::qrcode_confirm;
         } },
        { "hk4e_cn", [this]() {
             gameType = GameType::Genshin;
             scanUrl = api::mhy::hk4e::qrcode_scan;
             confirmUrl = api::mhy::hk4e::qrcode_confirm;
         } },
        { "hkrpg_cn", [this]() {
             gameType = GameType::HonkaiStarRail;
             scanUrl = api::mhy::hkrpg::qrcode_scan;
             confirmUrl = api::mhy::hkrpg::qrcode_confirm;
         } },
        { "nap_cn", [this]() {
             gameType = GameType::ZenlessZoneZero;
             scanUrl = api::mhy::nap::qrcode_scan;
             confirmUrl = api::mhy::nap::qrcode_confirm;
         } },
    };

    [[nodiscard]] bool setGameTypeByAppId(const std::string_view appId)
    {
        if (appId == "1")
        {
            setGameTypeByBizKey["bh3_cn"]();
            return true;
        }
        if (appId == "4")
        {
            setGameTypeByBizKey["hk4e_cn"]();
            return true;
        }
        if (appId == "8")
        {
            setGameTypeByBizKey["hkrpg_cn"]();
            return true;
        }
        if (appId == "12")
        {
            setGameTypeByBizKey["nap_cn"]();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseOfficialQRCode(const std::string_view qrCode, std::string& ticket, bool& isWebLogin)
    {
        isWebLogin = false;
        ticket = getUrlQueryParam(qrCode, "ticket");
        if (ticket.empty())
        {
            ticket = getUrlQueryParam(qrCode, "tk");
        }
        if (ticket.empty())
        {
            return false;
        }

        // 网页端登录二维码(user.mihoyo.com/login-platform, 含tk/token_types, 无biz_key/app_id):
        // 走通行证stoken确认流程, 无需游戏biz
        if (!getUrlQueryParam(qrCode, "token_types").empty() &&
            getUrlQueryParam(qrCode, "biz_key").empty() &&
            getUrlQueryParam(qrCode, "game_biz").empty() &&
            getUrlQueryParam(qrCode, "app_id").empty())
        {
            isWebLogin = true;
            return true;
        }

        std::string bizKey = getUrlQueryParam(qrCode, "biz_key");
        if (bizKey.empty())
        {
            bizKey = getUrlQueryParam(qrCode, "game_biz");
        }
        if (auto it = setGameTypeByBizKey.find(bizKey); it != setGameTypeByBizKey.end())
        {
            it->second();
            return true;
        }

        const std::string appId = getUrlQueryParam(qrCode, "app_id");
        return setGameTypeByAppId(appId);
    }
};