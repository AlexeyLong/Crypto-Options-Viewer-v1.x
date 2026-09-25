#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <gdiplus.h>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cmath>
#include "json.hpp"

#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Wininet.lib")

using json = nlohmann::json;
using namespace Gdiplus;

// ────────────────────────────────────────────────────────────
// SETTINGS
// ────────────────────────────────────────────────────────────
constexpr double BTC_MIN_STRIKE = 30000.0;
constexpr double BTC_MAX_STRIKE = 120000.0;
constexpr double ETH_MIN_STRIKE = 1000.0;
constexpr double ETH_MAX_STRIKE = 5500.0;

struct Theme {
    Color Background;
    Color MainText;
    Color SecondaryText;
    Color InfoText;
    Color Grid;
    Color Axis;
    Color CheckboxText;
};

const Theme g_Theme = {
    Color(255, 0, 0, 0),
    Color(255, 255, 255, 255),
    Color(255, 200, 200, 200),
    Color(255, 230, 230, 230),
    Color(255, 50, 50, 50),
    Color(255, 180, 180, 180),
    Color(255, 255, 255, 255)
};

// ────────────────────────────────────────────────────────────
// ENUMS
// ────────────────────────────────────────────────────────────
enum class Asset { BTC, ETH };
enum class Exchange { Deribit, Binance, OKX };
enum class DisplayMode { TotalOI, CallPut };

// ────────────────────────────────────────────────────────────
// CONTROL IDS
// ────────────────────────────────────────────────────────────
#define ID_CHK_TOTAL       1001
#define ID_CHK_CALLPUT     1002
#define ID_CHK_DERIBIT     1101
#define ID_CHK_BINANCE     1102
#define ID_CHK_OKX         1103
#define ID_CHK_PUT         1201
#define ID_CHK_CALL        1202
#define ID_ASSET_BTC       1301
#define ID_ASSET_ETH       1302

// ────────────────────────────────────────────────────────────
// DISPLAY SETTINGS
// ────────────────────────────────────────────────────────────
DisplayMode g_DisplayMode = DisplayMode::TotalOI;

bool g_ShowDeribit = true;
bool g_ShowBinance = true;
bool g_ShowOKX = true;
bool g_ShowPut = true;
bool g_ShowCall = true;

Asset g_CurrentAsset = Asset::BTC;

// ────────────────────────────────────────────────────────────
// DATA STRUCTURES
// ────────────────────────────────────────────────────────────
struct ExchangeOI {
    double callOI = 0.0;
    double putOI = 0.0;
};

struct StrikeData {
    ExchangeOI deribit;
    ExchangeOI binance;
    ExchangeOI okx;

    double totalCallOI = 0.0;
    double totalPutOI = 0.0;
};

struct SourceStatus {
    bool deribit = false;
    bool binance = false;
    bool okx = false;

    std::string deribitInfo;
    std::string binanceInfo;
    std::string okxInfo;
};

struct AssetCache {
    std::map<double, StrikeData> options;
    SourceStatus status;
    bool loaded = false;
    bool loading = false;
};

AssetCache g_BtcCache;
AssetCache g_EthCache;

std::mutex g_CacheMutex;

bool g_DataLoaded = false;

// ────────────────────────────────────────────────────────────
// HTTP GET
// ────────────────────────────────────────────────────────────
std::string HttpGet(const std::string& url)
{
    HINTERNET hInternet = InternetOpenA(
        "CryptoOptionsOIViewer/2.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        NULL,
        NULL,
        0
    );

    if (!hInternet)
        throw std::runtime_error(
            "InternetOpenA failed: " +
            std::to_string(GetLastError())
        );

    HINTERNET hConnect = InternetOpenUrlA(
        hInternet,
        url.c_str(),
        NULL,
        0,
        INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_SECURE,
        30000
    );

    if (!hConnect) {
        DWORD error = GetLastError();
        InternetCloseHandle(hInternet);

        throw std::runtime_error(
            "InternetOpenUrlA failed: " +
            std::to_string(error)
        );
    }

    std::string response;
    char buffer[8192];
    DWORD bytesRead = 0;

    while (true) {
        BOOL ok = InternetReadFile(
            hConnect,
            buffer,
            sizeof(buffer),
            &bytesRead
        );

        if (!ok) {
            DWORD error = GetLastError();

            InternetCloseHandle(hConnect);
            InternetCloseHandle(hInternet);

            throw std::runtime_error(
                "InternetReadFile failed: " +
                std::to_string(error)
            );
        }

        if (bytesRead == 0)
            break;

        response.append(buffer, bytesRead);
    }

    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    if (response.empty())
        throw std::runtime_error("HTTP response is empty");

    return response;
}

// ────────────────────────────────────────────────────────────
// JSON NUMBER
// ────────────────────────────────────────────────────────────
double JsonNumber(const json& value)
{
    if (value.is_number())
        return value.get<double>();

    if (value.is_string())
        return std::stod(value.get<std::string>());

    throw std::runtime_error(
        "JSON value is not number/string"
    );
}

// ────────────────────────────────────────────────────────────
// ASSET HELPERS
// ────────────────────────────────────────────────────────────
std::string AssetName(Asset asset)
{
    return asset == Asset::BTC ? "BTC" : "ETH";
}

AssetCache& GetCache(Asset asset)
{
    return asset == Asset::BTC
        ? g_BtcCache
        : g_EthCache;
}

const AssetCache& GetCacheConst(Asset asset)
{
    return asset == Asset::BTC
        ? g_BtcCache
        : g_EthCache;
}

bool IsStrikeAllowed(Asset asset, double strike)
{
    if (!std::isfinite(strike))
        return false;

    if (asset == Asset::BTC)
        return strike >= BTC_MIN_STRIKE &&
        strike <= BTC_MAX_STRIKE;

    return strike >= ETH_MIN_STRIKE &&
        strike <= ETH_MAX_STRIKE;
}

// ────────────────────────────────────────────────────────────
// ADD OI TO CACHE
// ────────────────────────────────────────────────────────────
void AddOptionOI(
    Asset asset,
    double strike,
    double oi,
    bool isCall,
    Exchange exchange
)
{
    if (!std::isfinite(strike))
        return;

    if (!std::isfinite(oi))
        return;

    if (oi <= 0.0)
        return;

    if (!IsStrikeAllowed(asset, strike))
        return;

    std::lock_guard<std::mutex> lock(g_CacheMutex);

    AssetCache& cache = GetCache(asset);

    StrikeData& data =
        cache.options[strike];

    ExchangeOI* exchangeData = nullptr;

    switch (exchange) {
    case Exchange::Deribit:
        exchangeData = &data.deribit;
        break;

    case Exchange::Binance:
        exchangeData = &data.binance;
        break;

    case Exchange::OKX:
        exchangeData = &data.okx;
        break;
    }

    if (!exchangeData)
        return;

    if (isCall) {
        exchangeData->callOI += oi;
        data.totalCallOI += oi;
    }
    else {
        exchangeData->putOI += oi;
        data.totalPutOI += oi;
    }
}

// ────────────────────────────────────────────────────────────
// DERIBIT OI
// ────────────────────────────────────────────────────────────
void ParseDeribit(Asset asset)
{
    try {
        const std::string currency =
            AssetName(asset);

        const std::string url =
            "https://www.deribit.com/api/v2/public/"
            "get_book_summary_by_currency"
            "?currency=" + currency +
            "&kind=option";

        std::string raw =
            HttpGet(url);

        json root =
            json::parse(raw);

        if (!root.contains("result") ||
            !root["result"].is_array())
        {
            throw std::runtime_error(
                "Deribit: invalid result"
            );
        }

        int callCount = 0;
        int putCount = 0;

        double totalCallOI = 0.0;
        double totalPutOI = 0.0;

        for (const auto& item : root["result"]) {
            if (!item.contains("instrument_name"))
                continue;

            if (!item.contains("open_interest"))
                continue;

            std::string name =
                item["instrument_name"].get<std::string>();

            if (name.empty())
                continue;

            bool isCall = false;

            if (name.back() == 'C')
                isCall = true;
            else if (name.back() == 'P')
                isCall = false;
            else
                continue;

            size_t lastDash =
                name.find_last_of('-');

            if (lastDash == std::string::npos)
                continue;

            size_t prevDash =
                name.find_last_of('-', lastDash - 1);

            if (prevDash == std::string::npos)
                continue;

            std::string strikeText =
                name.substr(
                    prevDash + 1,
                    lastDash - prevDash - 1
                );

            double strike = 0.0;

            try {
                strike = std::stod(strikeText);
            }
            catch (...) {
                continue;
            }

            double oi = 0.0;

            try {
                oi = JsonNumber(
                    item["open_interest"]
                );
            }
            catch (...) {
                continue;
            }

            if (!IsStrikeAllowed(asset, strike))
                continue;

            AddOptionOI(
                asset,
                strike,
                oi,
                isCall,
                Exchange::Deribit
            );

            if (isCall) {
                callCount++;
                totalCallOI += oi;
            }
            else {
                putCount++;
                totalPutOI += oi;
            }
        }

        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        AssetCache& cache =
            GetCache(asset);

        cache.status.deribit = true;

        std::ostringstream ss;

        ss << "CALL: " << callCount
            << " PUT: " << putCount
            << " | CALL OI: "
            << std::fixed
            << std::setprecision(2)
            << totalCallOI
            << " | PUT OI: "
            << totalPutOI;

        cache.status.deribitInfo =
            ss.str();
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        AssetCache& cache =
            GetCache(asset);

        cache.status.deribit = false;
        cache.status.deribitInfo =
            e.what();
    }
}

// ────────────────────────────────────────────────────────────
// BINANCE OI
// ────────────────────────────────────────────────────────────
void ParseBinance(Asset asset)
{
    try {
        const std::string symbolAsset =
            AssetName(asset);

        const std::string exchangeInfoUrl =
            "https://eapi.binance.com/"
            "eapi/v1/exchangeInfo";

        std::string raw =
            HttpGet(exchangeInfoUrl);

        json root =
            json::parse(raw);

        if (!root.contains("optionSymbols") ||
            !root["optionSymbols"].is_array())
        {
            throw std::runtime_error(
                "Binance: optionSymbols missing"
            );
        }

        std::vector<std::string> expirations;

        for (const auto& item :
            root["optionSymbols"])
        {
            if (!item.contains("symbol"))
                continue;

            if (!item.contains("underlying"))
                continue;

            std::string underlying =
                item["underlying"].get<std::string>();

            if (underlying != symbolAsset + "USDT")
                continue;

            if (!item.contains("expiryDate"))
                continue;

            long long expiry =
                item["expiryDate"].get<long long>();

            std::time_t timeValue =
                static_cast<std::time_t>(
                    expiry / 1000
                    );

            std::tm utc = {};

            gmtime_s(
                &utc,
                &timeValue
            );

            char dateBuffer[32] = {};

            std::strftime(
                dateBuffer,
                sizeof(dateBuffer),
                "%y%m%d",
                &utc
            );

            std::string expiration =
                dateBuffer;

            if (std::find(
                expirations.begin(),
                expirations.end(),
                expiration
            ) == expirations.end())
            {
                expirations.push_back(
                    expiration
                );
            }
        }

        if (expirations.empty())
            throw std::runtime_error(
                "Binance: no expirations found"
            );

        int callCount = 0;
        int putCount = 0;

        double totalCallOI = 0.0;
        double totalPutOI = 0.0;

        for (const std::string& expiration :
            expirations)
        {
            std::string url =
                "https://eapi.binance.com/"
                "eapi/v1/openInterest"
                "?underlyingAsset=" +
                symbolAsset +
                "&expiration=" +
                expiration;

            std::string rawOI =
                HttpGet(url);

            json oiData =
                json::parse(rawOI);

            if (!oiData.is_array())
                continue;

            for (const auto& item :
                oiData)
            {
                if (!item.contains("symbol"))
                    continue;

                if (!item.contains("sumOpenInterest"))
                    continue;

                std::string symbol =
                    item["symbol"].get<std::string>();

                if (symbol.rfind(
                    symbolAsset + "-",
                    0
                ) != 0)
                {
                    continue;
                }

                bool isCall = false;

                if (!symbol.empty() &&
                    symbol.back() == 'C')
                {
                    isCall = true;
                }
                else if (!symbol.empty() &&
                    symbol.back() == 'P')
                {
                    isCall = false;
                }
                else {
                    continue;
                }

                size_t firstDash =
                    symbol.find('-');

                size_t secondDash =
                    symbol.find(
                        '-',
                        firstDash + 1
                    );

                size_t thirdDash =
                    symbol.find(
                        '-',
                        secondDash + 1
                    );

                if (thirdDash ==
                    std::string::npos)
                {
                    continue;
                }

                std::string strikeText =
                    symbol.substr(
                        secondDash + 1,
                        thirdDash -
                        secondDash - 1
                    );

                double strike = 0.0;

                try {
                    strike =
                        std::stod(strikeText);
                }
                catch (...) {
                    continue;
                }

                double oi = 0.0;

                try {
                    oi = JsonNumber(
                        item["sumOpenInterest"]
                    );
                }
                catch (...) {
                    continue;
                }

                if (!IsStrikeAllowed(
                    asset,
                    strike))
                {
                    continue;
                }

                AddOptionOI(
                    asset,
                    strike,
                    oi,
                    isCall,
                    Exchange::Binance
                );

                if (isCall) {
                    callCount++;
                    totalCallOI += oi;
                }
                else {
                    putCount++;
                    totalPutOI += oi;
                }
            }
        }

        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        AssetCache& cache =
            GetCache(asset);

        cache.status.binance = true;

        std::ostringstream ss;

        ss << "EXP: "
            << expirations.size()
            << " | CALL: "
            << callCount
            << " PUT: "
            << putCount
            << " | CALL OI: "
            << std::fixed
            << std::setprecision(2)
            << totalCallOI
            << " | PUT OI: "
            << totalPutOI;

        cache.status.binanceInfo =
            ss.str();
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        AssetCache& cache =
            GetCache(asset);

        cache.status.binance = false;
        cache.status.binanceInfo =
            e.what();
    }
}

// ────────────────────────────────────────────────────────────
// OKX OI
// ────────────────────────────────────────────────────────────
void ParseOkx(Asset asset)
{
    try {
        const std::string currency =
            AssetName(asset);

        const std::string family =
            currency + "-USD";

        const std::string instrumentsUrl =
            "https://www.okx.com/api/v5/public/"
            "instruments"
            "?instType=OPTION"
            "&instFamily=" +
            family;

        std::string rawInstruments =
            HttpGet(instrumentsUrl);

        json instruments =
            json::parse(rawInstruments);

        if (!instruments.contains("code") ||
            instruments["code"].get<std::string>() != "0")
        {
            throw std::runtime_error(
                "OKX instruments API error"
            );
        }

        if (!instruments.contains("data") ||
            !instruments["data"].is_array())
        {
            throw std::runtime_error(
                "OKX instruments data missing"
            );
        }

        struct InstrumentInfo {
            double strike = 0.0;
            bool isCall = false;
            bool isPut = false;
        };

        std::map<std::string, InstrumentInfo>
            instrumentMap;

        for (const auto& item :
            instruments["data"])
        {
            if (!item.contains("instId"))
                continue;

            if (!item.contains("stk"))
                continue;

            if (!item.contains("optType"))
                continue;

            std::string instId =
                item["instId"].get<std::string>();

            std::string optType =
                item["optType"].get<std::string>();

            bool isCall = false;
            bool isPut = false;

            if (optType == "C")
                isCall = true;
            else if (optType == "P")
                isPut = true;
            else
                continue;

            double strike = 0.0;

            try {
                strike =
                    JsonNumber(item["stk"]);
            }
            catch (...) {
                continue;
            }

            if (!IsStrikeAllowed(
                asset,
                strike))
            {
                continue;
            }

            InstrumentInfo info;

            info.strike = strike;
            info.isCall = isCall;
            info.isPut = isPut;

            instrumentMap[instId] =
                info;
        }

        if (instrumentMap.empty())
            throw std::runtime_error(
                "OKX: no option instruments"
            );

        const std::string oiUrl =
            "https://www.okx.com/api/v5/public/"
            "open-interest"
            "?instType=OPTION"
            "&instFamily=" +
            family;

        std::string rawOI =
            HttpGet(oiUrl);

        json oiJson =
            json::parse(rawOI);

        if (!oiJson.contains("code") ||
            oiJson["code"].get<std::string>() != "0")
        {
            std::string msg;

            if (oiJson.contains("msg"))
                msg =
                oiJson["msg"].get<std::string>();

            throw std::runtime_error(
                "OKX OI API error: " + msg
            );
        }

        if (!oiJson.contains("data") ||
            !oiJson["data"].is_array())
        {
            throw std::runtime_error(
                "OKX OI data missing"
            );
        }

        int callCount = 0;
        int putCount = 0;

        double totalCallOI = 0.0;
        double totalPutOI = 0.0;

        for (const auto& item :
            oiJson["data"])
        {
            if (!item.contains("instId"))
                continue;

            std::string instId =
                item["instId"].get<std::string>();

            auto it =
                instrumentMap.find(instId);

            if (it == instrumentMap.end())
                continue;

            double oi = 0.0;

            try {
                if (item.contains("oiCcy") &&
                    !item["oiCcy"].is_null() &&
                    item["oiCcy"].is_string() &&
                    !item["oiCcy"].get<std::string>().empty())
                {
                    oi =
                        JsonNumber(item["oiCcy"]);
                }
                else if (item.contains("oi")) {
                    oi =
                        JsonNumber(item["oi"]);
                }
            }
            catch (...) {
                continue;
            }

            if (!std::isfinite(oi) ||
                oi <= 0.0)
            {
                continue;
            }

            const InstrumentInfo& info =
                it->second;

            AddOptionOI(
                asset,
                info.strike,
                oi,
                info.isCall,
                Exchange::OKX
            );

            if (info.isCall) {
                callCount++;
                totalCallOI += oi;
            }
            else if (info.isPut) {
                putCount++;
                totalPutOI += oi;
            }
        }

        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        AssetCache& cache =
            GetCache(asset);

        cache.status.okx = true;

        std::ostringstream ss;

        ss << "INSTR: "
            << instrumentMap.size()
            << " | CALL: "
            << callCount
            << " PUT: "
            << putCount
            << " | CALL OI: "
            << std::fixed
            << std::setprecision(2)
            << totalCallOI
            << " | PUT OI: "
            << totalPutOI;

        cache.status.okxInfo =
            ss.str();
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        AssetCache& cache =
            GetCache(asset);

        cache.status.okx = false;
        cache.status.okxInfo =
            e.what();
    }
}

// ────────────────────────────────────────────────────────────
// LOAD ONE ASSET
// ────────────────────────────────────────────────────────────
void LoadAsset(Asset asset)
{
    {
        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        AssetCache& cache =
            GetCache(asset);

        cache.options.clear();
        cache.status = SourceStatus{};
        cache.loaded = false;
        cache.loading = true;
    }

    std::thread deribit(
        ParseDeribit,
        asset
    );

    std::thread binance(
        ParseBinance,
        asset
    );

    std::thread okx(
        ParseOkx,
        asset
    );

    deribit.join();
    binance.join();
    okx.join();

    {
        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        AssetCache& cache =
            GetCache(asset);

        cache.loading = false;
        cache.loaded = true;
    }
}

// ────────────────────────────────────────────────────────────
// INITIAL LOAD - ONLY ON APPLICATION START
// ────────────────────────────────────────────────────────────
void AsyncLoadData(HWND hwnd)
{
    {
        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        g_BtcCache = AssetCache{};
        g_EthCache = AssetCache{};

        g_DataLoaded = false;
    }

    LoadAsset(Asset::BTC);
    LoadAsset(Asset::ETH);

    {
        std::lock_guard<std::mutex> lock(
            g_CacheMutex
        );

        g_DataLoaded =
            g_BtcCache.loaded &&
            g_EthCache.loaded;
    }

    if (IsWindow(hwnd))
        InvalidateRect(
            hwnd,
            NULL,
            TRUE
        );
}

// ────────────────────────────────────────────────────────────
// DRAW TEXT
// ────────────────────────────────────────────────────────────

void DrawTextGdi(
    Graphics& graphics,
    const wchar_t* text,
    float x,
    float y,
    float size,
    Color color
)
{
    FontFamily fontFamily(L"Arial");

    Font font(
        &fontFamily,
        size,
        FontStyleRegular,
        UnitPixel
    );

    SolidBrush brush(color);

    PointF point(x, y);

    graphics.DrawString(
        text,
        -1,
        &font,
        point,
        &brush
    );
}

// ────────────────────────────────────────────────────────────
// DRAW CHECKBOX
// ────────────────────────────────────────────────────────────
void DrawCheckbox(Graphics& graphics, float x, float y, bool checked, const wchar_t* text)
{
    Pen borderPen(Color(255, 100, 110, 120), 1.2f);
    SolidBrush whiteBrush(Color(255, 255, 255, 255));
    graphics.FillRectangle(&whiteBrush, x, y, 15.0f, 15.0f);
    graphics.DrawRectangle(&borderPen, x, y, 15.0f, 15.0f);
    if (checked) {
        Pen checkPen(Color(255, 35, 120, 80), 2.0f);
        graphics.DrawLine(&checkPen, x + 3.0f, y + 8.0f, x + 6.0f, y + 12.0f);
        graphics.DrawLine(&checkPen, x + 6.0f, y + 12.0f, x + 13.0f, y + 3.0f);
    }
    DrawTextGdi(graphics, text, x + 22.0f, y - 2.0f, 12.0f, g_Theme.CheckboxText);
}

// ────────────────────────────────────────────────────────────
// SELECTED EXCHANGE OI
// ────────────────────────────────────────────────────────────
ExchangeOI GetSelectedOI(const StrikeData& data)
{
    ExchangeOI result;
    if (g_ShowDeribit) {
        result.callOI += data.deribit.callOI;
        result.putOI += data.deribit.putOI;
    }
    if (g_ShowBinance) {
        result.callOI += data.binance.callOI;
        result.putOI += data.binance.putOI;
    }
    if (g_ShowOKX) {
        result.callOI += data.okx.callOI;
        result.putOI += data.okx.putOI;
    }
    return result;

}
// ────────────────────────────────────────────────────────────
// GET CURRENT CACHE SNAPSHOT
// ────────────────────────────────────────────────────────────
std::map<double, StrikeData>
GetCurrentData()
{
    std::lock_guard<std::mutex> lock(g_CacheMutex); return GetCacheConst(g_CurrentAsset).options;
}

// ────────────────────────────────────────────────────────────
// GET CURRENT STATUS
// ────────────────────────────────────────────────────────────
SourceStatus GetCurrentStatus()
{
    std::lock_guard<std::mutex> lock(g_CacheMutex); return GetCacheConst(g_CurrentAsset).status;
}

// ────────────────────────────────────────────────────────────
// PAINT
// ────────────────────────────────────────────────────────────
void OnPaint(HWND hwnd, HDC hdc)
{
    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.Clear(g_Theme.Background);
    RECT rect;
    GetClientRect(hwnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    std::map<double, StrikeData> data = GetCurrentData();
    SourceStatus status = GetCurrentStatus();
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        loaded = GetCacheConst(g_CurrentAsset).loaded;
    }

    // Menu
    DrawCheckbox(graphics, 700, 20, g_CurrentAsset == Asset::BTC, L"BTC");
    DrawCheckbox(graphics, 790, 20, g_CurrentAsset == Asset::ETH, L"ETH");

    std::wstring title =
        g_CurrentAsset == Asset::BTC
        ? L"Open Interest By Strike Price (BTC)"
        : L"Open Interest By Strike Price (ETH)";
    DrawTextGdi(graphics, title.c_str(), 25, 15, 18, g_Theme.MainText);
    DrawCheckbox(graphics, 400, 20, g_DisplayMode == DisplayMode::TotalOI, L"Total OI");
    DrawCheckbox(graphics, 510, 20, g_DisplayMode == DisplayMode::CallPut, L"Call / Put");

    std::wstring statusText;

    if (!loaded) {
        statusText =
            L"Loading OI cache...";
    }
    else {
        statusText =
            L"Cache loaded no live updates";
    }
    DrawTextGdi(graphics, statusText.c_str(), 27, 50, 14, g_Theme.SecondaryText);

    std::wstring exchangeStatus;

    exchangeStatus += L"Deribit: ";
    exchangeStatus +=
        status.deribit
        ? L"OK"
        : L"ERROR";

    exchangeStatus +=
        L"    Binance: ";

    exchangeStatus +=
        status.binance
        ? L"OK"
        : L"ERROR";

    exchangeStatus +=
        L"    OKX: ";

    exchangeStatus +=
        status.okx
        ? L"OK"
        : L"ERROR";

    DrawTextGdi(
        graphics,
        exchangeStatus.c_str(),
        27,
        73,
        13,
        g_Theme.SecondaryText
    );

    if (!loaded) {
        DrawTextGdi(
            graphics,
            L"Building cache...",
            50,
            150,
            18,
            Color(255, 30, 100, 180)
        );

        return;
    }

    if (data.empty()) {
        DrawTextGdi(
            graphics,
            L"No OI data",
            50,
            150,
            18,
            Color(255, 200, 50, 50)
        );

        return;
    }

    // OI
    double totalCalls = 0.0;
    double totalPuts = 0.0;

    for (const auto& [strike, option] : data) {
        ExchangeOI selected =
            GetSelectedOI(option);

        if (g_ShowCall)
            totalCalls +=
            selected.callOI;

        if (g_ShowPut)
            totalPuts +=
            selected.putOI;
    }

    double totalOI =
        totalCalls + totalPuts;

    double putCallRatio = 0.0;

    if (totalCalls > 0.0)
        putCallRatio =
        totalPuts / totalCalls;

    std::wstringstream info;

    info << L"CALL OI: "
        << std::fixed
        << std::setprecision(2)
        << totalCalls

        << L"    PUT OI: "
        << totalPuts

        << L"    TOTAL OI: "
        << totalOI

        << L"    Put / Call: "
        << std::setprecision(2)
        << putCallRatio;

    DrawTextGdi(
        graphics,
        info.str().c_str(),
        27,
        100,
        13,
        g_Theme.InfoText
    );

    // Max
    double maxOI = 0.0;

    for (const auto& [strike, option] : data) {
        ExchangeOI selected =
            GetSelectedOI(option);

        double callOI =
            g_ShowCall
            ? selected.callOI
            : 0.0;

        double putOI =
            g_ShowPut
            ? selected.putOI
            : 0.0;

        double value = 0.0;

        if (g_DisplayMode ==
            DisplayMode::TotalOI)
        {
            value =
                callOI + putOI;
        }
        else {
            value =
                std::max(
                    callOI,
                    putOI
                );
        }

        if (value > maxOI)
            maxOI = value;
    }

    if (maxOI <= 0.0)
        maxOI = 1.0;

    const int leftMargin = 70;
    const int rightMargin = 30;
    const int topMargin = 140;
    const int bottomMargin = 125;

    int graphWidth =
        width -
        leftMargin -
        rightMargin;

    int graphHeight =
        height -
        topMargin -
        bottomMargin;

    if (graphWidth <= 0 ||
        graphHeight <= 0)
        return;

    // Сетка
    Pen gridPen(
        g_Theme.Grid,
        1.0f
    );

    for (int i = 0; i <= 5; ++i) {
        int y =
            topMargin +
            graphHeight * i / 5;

        graphics.DrawLine(
            &gridPen,
            leftMargin,
            y,
            width - rightMargin,
            y
        );

        double value =
            maxOI *
            (5 - i) /
            5.0;

        std::wstringstream ss;

        ss << std::fixed
            << std::setprecision(1)
            << value;

        DrawTextGdi(
            graphics,
            ss.str().c_str(),
            10,
            static_cast<float>(y - 6),
            10,
            g_Theme.SecondaryText
        );
    }

    Pen axisPen(
        g_Theme.Axis,
        1.5f
    );

    graphics.DrawLine(
        &axisPen,
        leftMargin,
        topMargin,
        leftMargin,
        height - bottomMargin
    );

    graphics.DrawLine(
        &axisPen,
        leftMargin,
        height - bottomMargin,
        width - rightMargin,
        height - bottomMargin
    );

    int count =
        static_cast<int>(
            data.size()
            );

    if (count <= 0)
        return;

    float groupWidth =
        static_cast<float>(
            graphWidth
            ) /
        static_cast<float>(
            count
            );

    // Colors
    SolidBrush callBrush(Color(255, 0, 0, 255));
    SolidBrush putBrush(Color(255, 255, 0, 0));
    SolidBrush totalBrush(Color(255, 80, 80, 80));
    SolidBrush callHighBrush(Color(255, 0, 0, 255));
    SolidBrush putHighBrush(Color(255, 255, 0, 0));
    SolidBrush totalHighBrush(Color(255, 80, 80, 80));
    int index = 0;
    int labelStep = std::max(1, count / 18);

    // Graps
    for (const auto& [strike, option] : data)
    {
        ExchangeOI selected = GetSelectedOI(option);
        double callOI = g_ShowCall ? selected.callOI : 0.0;
        double putOI = g_ShowPut ? selected.putOI : 0.0;
        if (g_DisplayMode == DisplayMode::TotalOI) {
            double total = callOI + putOI;
            if (total > 0.0) {
                float normalized = static_cast<float>(total / maxOI);
                normalized = std::min(normalized, 1.0f);
                float barHeight = normalized * static_cast<float>(graphHeight);
                float barWidth = groupWidth * 0.50f;
                if (barWidth < 8.0f) barWidth = 8.0f;
                float x = static_cast<float>(leftMargin) + index * groupWidth + (groupWidth - barWidth) / 2.0f;
                float y = static_cast<float>(height - bottomMargin) - barHeight;
                SolidBrush* brush = normalized > 0.75f ? &totalHighBrush : &totalBrush;
                graphics.FillRectangle(brush, x, y, barWidth, barHeight);
            }
        }
        else {
            float barWidth = groupWidth * 0.38f;
            if (barWidth < 5.0f) barWidth = 5.0f;
            if (g_ShowCall && callOI > 0.0) {
                float normalized = static_cast<float>(callOI / maxOI);
                normalized = std::min(normalized, 1.0f);
                float barHeight = normalized * static_cast<float>(graphHeight);
                float x = static_cast<float>(leftMargin) + index * groupWidth + groupWidth * 0.08f;
                float y = static_cast<float>(height - bottomMargin) - barHeight;
                SolidBrush* brush = normalized > 0.75f ? &callHighBrush : &callBrush;
                graphics.FillRectangle(brush, x, y, barWidth, barHeight);
            }
            if (g_ShowPut && putOI > 0.0) {
                float normalized = static_cast<float>(putOI / maxOI);
                normalized = std::min(normalized, 1.0f);
                float barHeight = normalized * static_cast<float>(graphHeight);
                float x = static_cast<float>(leftMargin) + index * groupWidth + groupWidth * 0.54f;
                float y = static_cast<float>(height - bottomMargin) - barHeight;
                SolidBrush* brush = normalized > 0.75f ? &putHighBrush : &putBrush;
                graphics.FillRectangle(brush, x, y, barWidth, barHeight);
            }
        }
        if (index % labelStep == 0) {
            std::wstring text = std::to_wstring(static_cast<long long>(strike));
            float x = static_cast<float>(leftMargin) + index * groupWidth;
            DrawTextGdi(graphics, text.c_str(), x, static_cast<float>(height - bottomMargin + 8), 10, g_Theme.SecondaryText);
        }
        ++index;
    }
    if (g_DisplayMode == DisplayMode::CallPut)
    {
        if (g_ShowCall) {
            SolidBrush brush(Color(255, 0, 0, 255));
            graphics.FillRectangle(&brush, 27, height - 87, 12, 12);
            DrawTextGdi(graphics, L"CALL OI", 45, height - 90, 12, g_Theme.MainText);
        }
        if (g_ShowPut) {
            SolidBrush brush(Color(255, 255, 0, 0));
            graphics.FillRectangle(&brush, 120, height - 87, 12, 12);
            DrawTextGdi(graphics, L"PUT OI", 138, height - 90, 12, g_Theme.MainText);
        }
    }

    // MaxStream
    std::wstringstream maxStream;
    maxStream
        << L"Max OI: "
        << std::fixed
        << std::setprecision(2)
        << maxOI;

    DrawTextGdi(
        graphics,
        maxStream.str().c_str(),
        10,
        static_cast<float>(
            topMargin - 20
            ),
        11,
        g_Theme.SecondaryText
    );

    // Strikes
    std::wstring countText =
        L"Strikes: " +
        std::to_wstring(count);

    DrawTextGdi(
        graphics,
        countText.c_str(),
        width - 160,
        50,
        13,
        g_Theme.SecondaryText
    );

    // Filters
    int exchangeY = height - 48;
    DrawCheckbox(graphics, 70, static_cast<float>(exchangeY), g_ShowDeribit, L"Deribit");
    DrawCheckbox(graphics, 220, static_cast<float>(exchangeY), g_ShowBinance, L"Binance");
    DrawCheckbox(graphics, 370, static_cast<float>(exchangeY), g_ShowOKX, L"OKX");
    DrawCheckbox(graphics, 560, static_cast<float>(exchangeY), g_ShowPut, L"Put");
    DrawCheckbox(graphics, 660, static_cast<float>(exchangeY), g_ShowCall, L"Call");
}

// ────────────────────────────────────────────────────────────
// WINDOW PROCEDURE
// ────────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        std::thread(AsyncLoadData, hwnd).detach();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp);
        int y = HIWORD(lp);
        if (x >= 700 && x <= 775 && y >= 15 && y <= 45) {
            g_CurrentAsset = Asset::BTC;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (x >= 790 && x <= 865 && y >= 15 && y <= 45) {
            g_CurrentAsset = Asset::ETH;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (x >= 350 && x <= 490 && y >= 15 && y <= 45) {
            g_DisplayMode = DisplayMode::TotalOI;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (x >= 500 && x <= 650 && y >= 15 && y <= 45) {
            g_DisplayMode = DisplayMode::CallPut;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        RECT rect;
        GetClientRect(hwnd, &rect);
        int height = rect.bottom - rect.top;
        int exchangeY = height - 55;
        if (x >= 60 && x <= 180 && y >= exchangeY && y <= exchangeY + 30) {
            g_ShowDeribit = !g_ShowDeribit;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (x >= 210 && x <= 330 && y >= exchangeY && y <= exchangeY + 30) {
            g_ShowBinance = !g_ShowBinance;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (x >= 360 && x <= 450 && y >= exchangeY && y <= exchangeY + 30) {
            g_ShowOKX = !g_ShowOKX;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (x >= 550 && x <= 640 && y >= exchangeY && y <= exchangeY + 30) {
            g_ShowPut = !g_ShowPut;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (x >= 650 && x <= 750 && y >= exchangeY && y <= exchangeY + 30) {
            g_ShowCall = !g_ShowCall;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        OnPaint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_DESTROY: PostQuitMessage(0);
        return 0;
    default: return DefWindowProc(hwnd, msg, wp, lp);
    }

}

// ────────────────────────────────────────────────────────────
// WINMAIN
// ────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow
)
{
    GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL);
    WNDCLASSW wc = {
    }
    ;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CryptoOptionsOIClass";
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        return 1;
    }
    HWND hwnd = CreateWindowW(L"CryptoOptionsOIClass", L"Crypto Options Viewer v1.x", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1400, 750, NULL, NULL, hInst, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        return 1;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    MSG msg = {
    }
    ;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(gdiplusToken);
    return static_cast<int>(msg.wParam);
}