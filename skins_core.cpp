#include <stdio.h>
#include "skins_core.h"

SkinsCore g_SkinsCore;
PLUGIN_EXPOSE(SkinsCore, g_SkinsCore);

IUtilsApi*          g_pUtils     = nullptr;
IMenusApi*          g_pMenus     = nullptr;
IPlayersApi*        g_pPlayers   = nullptr;
IResourcePrecacher* g_pPrecacher = nullptr;
IVIPApi*            g_pVIP       = nullptr;
IAdminApi*          g_pAdmin     = nullptr;

IVEngineServer2*   engine              = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem*     g_pEntitySystem     = nullptr;
IMySQLConnection*  g_pDB               = nullptr;

struct SkinEntry
{
    std::string              sId;
    std::string              sName;
    std::vector<std::string> vecModels_T;
    std::vector<std::string> vecModels_CT;
    std::vector<std::string> vecVIPGroups;
    std::vector<std::string> vecAdminFlags;
    std::vector<uint64>      vecSteamIDs;
};

struct AdminMenuState
{
    int         iTargetSlot = -1;
    std::string sSkinId;
};

struct PlayerData
{
    std::map<std::string, int64> mSkins;
    std::string                  sSkinId;
    std::string                  sDefaultModel[4];
    int                          iCooldownEnd;
};

std::map<std::string, SkinEntry>  g_Skins;
PlayerData                        g_Players[64];
AdminMenuState                    g_AdminState[64];

std::random_device g_RandDevice;
std::mt19937       g_RandGen(g_RandDevice());

static std::string              g_sDBName     = "skins_core";
static std::string              g_sDBHost;
static std::string              g_sDBUser;
static std::string              g_sDBPass;
static std::string              g_sDBBase;
static int                      g_iDBPort     = 3306;
static int                      g_iCooldown   = 30;
static float                    g_fApplyDelay = 0.0f;
static std::vector<std::string> g_vecChatCmds;
static std::vector<std::string> g_vecDefaultModels_T;
static std::vector<std::string> g_vecDefaultModels_CT;
CGameEntitySystem* GameEntitySystem() { return g_pUtils ? g_pUtils->GetCGameEntitySystem() : nullptr; }

static std::map<std::string, std::string> g_Phrases;

static const char* Phrase(const char* key, const char* def = "")
{
    auto it = g_Phrases.find(key);
    return (it != g_Phrases.end() && !it->second.empty()) ? it->second.c_str() : def;
}

static void LoadPhrases()
{
    g_Phrases.clear();
    KeyValues::AutoDelete kv("Phrases");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/translations/skins_core.phrases.txt"))
    {
        ConColorMsg(Color(255,255,0,255), "[SC] skins_core.phrases.txt not found\n");
        return;
    }
    const char* lang = g_pUtils ? g_pUtils->GetLanguage() : "ru";
    for (KeyValues* p = kv->GetFirstTrueSubKey(); p; p = p->GetNextTrueSubKey())
    {
        const char* val = p->GetString(lang, nullptr);
        if (!val) val = p->GetString("ru", p->GetName());
        if (val) g_Phrases[p->GetName()] = val;
    }
}



static std::vector<std::string> SplitString(const std::string& s, char delim)
{
    std::vector<std::string> out;
    std::string tok;
    for (char c : s)
    {
        if (c == delim) { if (!tok.empty()) { out.push_back(tok); tok.clear(); } }
        else tok += c;
    }
    if (!tok.empty()) out.push_back(tok);
    return out;
}

static const std::string& PickRandom(const std::vector<std::string>& vec)
{
    static const std::string sEmpty;
    if (vec.empty()) return sEmpty;
    if (vec.size() == 1) return vec[0];
    std::uniform_int_distribution<size_t> dist(0, vec.size() - 1);
    return vec[dist(g_RandGen)];
}

static bool PlayerCanAccessSkin(int iSlot, const std::string& skinId)
{
    auto it = g_Skins.find(skinId);
    if (it == g_Skins.end()) return false;
    const SkinEntry& skin = it->second;

    auto dit = g_Players[iSlot].mSkins.find(skinId);
    if (dit != g_Players[iSlot].mSkins.end())
        if (dit->second == 0 || dit->second > (int64)std::time(nullptr))
            return true;

    if (!skin.vecSteamIDs.empty())
    {
        uint64 steamid = g_pPlayers->GetSteamID64(iSlot);
        for (uint64 sid : skin.vecSteamIDs)
            if (sid == steamid) return true;
    }

    if (g_pVIP && !skin.vecVIPGroups.empty() && g_pVIP->VIP_IsClientVIP(iSlot))
    {
        const char* szGroup = g_pVIP->VIP_GetClientVIPGroup(iSlot);
        if (szGroup)
            for (const auto& g : skin.vecVIPGroups)
                if (g == szGroup) return true;
    }

    if (g_pAdmin && !skin.vecAdminFlags.empty())
        for (const auto& f : skin.vecAdminFlags)
            if (!f.empty())
                if (f[0] == '@' ? g_pAdmin->HasPermission(iSlot, f.c_str()) : g_pAdmin->HasFlag(iSlot, f.c_str()))
                    return true;

    return false;
}

static bool PlayerHasAnyAccessibleSkin(int iSlot)
{
    for (const auto& [id, skin] : g_Skins)
        if (PlayerCanAccessSkin(iSlot, id)) return true;
    return false;
}

static bool IsAdmin(int iSlot)
{
    if (!g_pAdmin) return false;
    return g_pAdmin->HasPermission(iSlot, "@admin/skins") || g_pAdmin->HasFlag(iSlot, "z");
}

static std::string FormatTime(int64 iSeconds)
{
    if (iSeconds <= 0) return "навсегда";
    int d = iSeconds / 86400, h = (iSeconds % 86400) / 3600,
        m = (iSeconds % 3600) / 60, s = iSeconds % 60;
    char buf[64];
    if (d > 0)      g_SMAPI->Format(buf, sizeof(buf), "%dд %dч", d, h);
    else if (h > 0) g_SMAPI->Format(buf, sizeof(buf), "%dч %dм", h, m);
    else if (m > 0) g_SMAPI->Format(buf, sizeof(buf), "%dм %dс", m, s);
    else            g_SMAPI->Format(buf, sizeof(buf), "%dс", s);
    return buf;
}

static void SetModel(CCSPlayerPawn* pPawn, const std::string& sModel)
{
    if (!pPawn || sModel.empty()) return;
    g_pUtils->SetEntityModel(pPawn, sModel.c_str());
    g_pUtils->SetStateChanged(pPawn, "CBaseModelEntity", "m_nModelIndex");
    g_pUtils->SetStateChanged(pPawn, "CBaseModelEntity", "m_ModelName");
    Vector vPos = pPawn->GetAbsOrigin();
    g_pUtils->TeleportEntity(pPawn, &vPos, nullptr, nullptr);
}

static void ApplySkinModel(int iSlot, const std::string& skinId)
{
    auto it = g_Skins.find(skinId);
    if (it == g_Skins.end()) return;

    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController) return;
    CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) return;

    int iTeam = pController->GetTeam();
    if (iTeam < 2 || iTeam > 3) return;

    if (g_Players[iSlot].sDefaultModel[iTeam].empty())
        g_Players[iSlot].sDefaultModel[iTeam] = pPawn->GetModelName().String();

    const std::string& sModel = (iTeam == 2)
        ? PickRandom(it->second.vecModels_T)
        : PickRandom(it->second.vecModels_CT);

    if (sModel.empty()) return;
    SetModel(pPawn, sModel);
}

static void ApplyDefaultSkin(int iSlot)
{
    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController) return;
    CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) return;

    int iTeam = pController->GetTeam();
    if (iTeam < 2 || iTeam > 3) return;

    const std::string& sModel = (iTeam == 2)
        ? PickRandom(g_vecDefaultModels_T)
        : PickRandom(g_vecDefaultModels_CT);
    if (sModel.empty()) return;

    if (g_Players[iSlot].sDefaultModel[iTeam].empty())
        g_Players[iSlot].sDefaultModel[iTeam] = pPawn->GetModelName().String();

    SetModel(pPawn, sModel);
}

static void ApplySkinOnSpawn(int iSlot)
{
    const std::string& cur = g_Players[iSlot].sSkinId;
    if (!cur.empty())
    {
        if (PlayerCanAccessSkin(iSlot, cur))
        {
            ApplySkinModel(iSlot, cur);
            return;
        }
        g_Players[iSlot].sSkinId = "";
    }
    ApplyDefaultSkin(iSlot);
}

static void RestoreDefault(int iSlot)
{
    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController) return;
    CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) return;
    int iTeam = pController->GetTeam();
    if (iTeam < 2 || iTeam > 3) return;
    if (g_Players[iSlot].sDefaultModel[iTeam].empty()) return;
    SetModel(pPawn, g_Players[iSlot].sDefaultModel[iTeam]);
}

static void DB_CreateTable()
{
    if (!g_pDB) return;
    g_pDB->Query(
        "CREATE TABLE IF NOT EXISTS `skins_ded_core` ("
        "  `steamid`     BIGINT UNSIGNED NOT NULL,"
        "  `skin_id`     VARCHAR(64)     NOT NULL,"
        "  `expire_time` BIGINT          NOT NULL DEFAULT 0,"
        "  `selected`    TINYINT         NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (`steamid`,`skin_id`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
        [](ISQLQuery*) {}
    );
    g_pDB->Query(
        "ALTER TABLE `skins_ded_core` ADD COLUMN IF NOT EXISTS `selected` TINYINT NOT NULL DEFAULT 0;",
        [](ISQLQuery*) {}
    );
}

static void DB_Load(int iSlot, uint64 steamid)
{
    if (!g_pDB) return;

    char szQ[256];
    g_SMAPI->Format(szQ, sizeof(szQ),
        "SELECT `skin_id`,`expire_time`,`selected` FROM `skins_ded_core` WHERE `steamid`='%llu';",
        steamid);

    g_pDB->Query(szQ, [iSlot, steamid](ISQLQuery* pQ) {
        ISQLResult* pR = pQ->GetResultSet();
        if (!pR || !pR->GetRowCount()) return;

        int64 iNow = (int64)std::time(nullptr);
        std::vector<std::string> expired;
        std::vector<std::string> staleMarkers;

        while (pR->FetchRow())
        {
            const char* szSkin   = pR->GetString(0);
            const char* szExpire = pR->GetString(1);
            int         iSel     = pR->GetInt(2);
            if (!szSkin || !szSkin[0]) continue;

            int64 iExpire = 0;
            try { iExpire = (szExpire && szExpire[0]) ? std::stoll(szExpire) : 0; } catch (...) {}

            if (iExpire == -1)
            {
                // Selection marker for VIP/admin/personal (not a DB-given skin)
                if (iSel && g_Skins.find(szSkin) != g_Skins.end())
                    g_Players[iSlot].sSkinId = szSkin;
                continue;
            }

            if (iExpire > 0 && iExpire <= iNow) { expired.push_back(szSkin); continue; }

            g_Players[iSlot].mSkins[szSkin] = iExpire;

            if (iSel && g_Skins.find(szSkin) != g_Skins.end())
                g_Players[iSlot].sSkinId = szSkin;
        }

        for (const auto& sid : expired)
        {
            if (!g_pDB) break;
            char szDel[512];
            g_SMAPI->Format(szDel, sizeof(szDel),
                "DELETE FROM `skins_ded_core` WHERE `steamid`='%llu' AND `skin_id`='%s';",
                steamid, sid.c_str());
            g_pDB->Query(szDel, [](ISQLQuery*) {});
        }
    });
}

static void DB_SaveSelected(uint64 steamid, const std::string& skinId)
{
    if (!g_pDB) return;
    char szQ[512];

    g_SMAPI->Format(szQ, sizeof(szQ),
        "UPDATE `skins_ded_core` SET `selected`=0 WHERE `steamid`='%llu';", steamid);
    g_pDB->Query(szQ, [](ISQLQuery*) {});

    g_SMAPI->Format(szQ, sizeof(szQ),
        "DELETE FROM `skins_ded_core` WHERE `steamid`='%llu' AND `expire_time`='-1';", steamid);
    g_pDB->Query(szQ, [](ISQLQuery*) {});

    if (skinId.empty()) return;

    g_SMAPI->Format(szQ, sizeof(szQ),
        "INSERT INTO `skins_ded_core`(`steamid`,`skin_id`,`expire_time`,`selected`) "
        "VALUES('%llu','%s','-1',1) "
        "ON DUPLICATE KEY UPDATE `selected`=1;",
        steamid, skinId.c_str());
    g_pDB->Query(szQ, [](ISQLQuery*) {});
}

static void DB_GiveSkin(uint64 steamid, const std::string& skinId, int64 iExpire)
{
    if (!g_pDB) return;
    char szQ[512];
    g_SMAPI->Format(szQ, sizeof(szQ),
        "INSERT INTO `skins_ded_core`(`steamid`,`skin_id`,`expire_time`,`selected`) "
        "VALUES('%llu','%s','%lld',0) "
        "ON DUPLICATE KEY UPDATE `expire_time`=VALUES(`expire_time`);",
        steamid, skinId.c_str(), iExpire);
    g_pDB->Query(szQ, [](ISQLQuery*) {});
}

static void DB_RemoveSkin(uint64 steamid, const std::string& skinId)
{
    if (!g_pDB) return;
    char szQ[256];
    g_SMAPI->Format(szQ, sizeof(szQ),
        "DELETE FROM `skins_ded_core` WHERE `steamid`='%llu' AND `skin_id`='%s';",
        steamid, skinId.c_str());
    g_pDB->Query(szQ, [](ISQLQuery*) {});
}

static void DB_RevokeAll(uint64 steamid)
{
    if (!g_pDB) return;
    char szQ[256];
    g_SMAPI->Format(szQ, sizeof(szQ),
        "DELETE FROM `skins_ded_core` WHERE `steamid`='%llu';", steamid);
    g_pDB->Query(szQ, [](ISQLQuery*) {});
}

static void DB_Connect()
{
    int ret;
    ISQLInterface* pSQL = (ISQLInterface*)g_SMAPI->MetaFactory(SQLMM_INTERFACE, &ret, nullptr);
    if (!pSQL || ret == META_IFACE_FAILED) { ConColorMsg(Color(255,0,0,255), "[SC] SQL interface not found\n"); return; }

    IMySQLClient* pClient = pSQL->GetMySQLClient();
    if (!pClient) { ConColorMsg(Color(255,0,0,255), "[SC] MySQL client not found\n"); return; }

    MySQLConnectionInfo info;
    info.host = g_sDBHost.c_str(); info.user = g_sDBUser.c_str();
    info.pass = g_sDBPass.c_str(); info.database = g_sDBBase.c_str();
    info.port = g_iDBPort;

    g_pDB = pClient->CreateMySQLConnection(info);
    if (!g_pDB) return;

    g_pDB->Connect([](bool bOK) {
        if (!bOK) { ConColorMsg(Color(255,0,0,255), "[SC] DB connection failed\n"); g_pDB = nullptr; return; }
        ConColorMsg(Color(0,255,0,255), "[SC] DB connected\n");
        DB_CreateTable();
    });
}

static void LoadDBCreds()
{
    KeyValues* kv = new KeyValues("Databases");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/configs/databases.cfg"))
    { ConColorMsg(Color(255,0,0,255), "[SC] databases.cfg not found\n"); delete kv; return; }

    KeyValues* pEntry = kv->FindKey(g_sDBName.c_str());
    if (!pEntry)
    { ConColorMsg(Color(255,0,0,255), "[SC] DB entry '%s' not found\n", g_sDBName.c_str()); delete kv; return; }

    g_sDBHost = pEntry->GetString("host",     "localhost");
    g_sDBUser = pEntry->GetString("user",     "root");
    g_sDBPass = pEntry->GetString("pass",     "");
    g_sDBBase = pEntry->GetString("database", "");
    g_iDBPort = pEntry->GetInt("port",         3306);
    delete kv;
}

static void LoadSkins()
{
    g_Skins.clear();
    KeyValues* kv = new KeyValues("Skins");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/configs/skins_core/skins.ini"))
    { ConColorMsg(Color(255,0,0,255), "[SC] skins.ini not found\n"); delete kv; return; }

    FOR_EACH_SUBKEY(kv, pSkin)
    {
        SkinEntry e;
        e.sId   = pSkin->GetName();
        e.sName = pSkin->GetString("name", e.sId.c_str());

        const char* szT  = pSkin->GetString("model_t",  "");
        const char* szCT = pSkin->GetString("model_ct", "");
        if (szT  && *szT)  e.vecModels_T  = SplitString(szT,  ',');
        if (szCT && *szCT) e.vecModels_CT = SplitString(szCT, ',');

        const char* szVIP   = pSkin->GetString("vip",      "");
        const char* szAdmin = pSkin->GetString("admin",    "");
        const char* szSIDs  = pSkin->GetString("steamids", "");

        if (szVIP   && *szVIP)   e.vecVIPGroups  = SplitString(szVIP,   ',');
        if (szAdmin && *szAdmin) e.vecAdminFlags  = SplitString(szAdmin, ',');
        if (szSIDs  && *szSIDs)
            for (const auto& s : SplitString(szSIDs, ','))
                try { e.vecSteamIDs.push_back(std::stoull(s)); } catch (...) {}

        g_Skins[e.sId] = e;
    }
    delete kv;
    ConColorMsg(Color(0,255,0,255), "[SC] Loaded %d skins\n", (int)g_Skins.size());
}

static void LoadConfig()
{
    KeyValues* kv = new KeyValues("SkinsCore");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/configs/skins_core/config.ini"))
    { ConColorMsg(Color(255,255,0,255), "[SC] config.ini not found, using defaults\n"); delete kv; return; }

    g_sDBName     = kv->GetString("database",   "skins_core");
    g_iCooldown   = kv->GetInt("cooldown",       30);
    g_fApplyDelay = kv->GetFloat("apply_delay",  0.0f);
    g_vecChatCmds = SplitString(kv->GetString("commands", "!skin,!skins"), ',');

    const char* szT  = kv->GetString("default_model_t",  "");
    const char* szCT = kv->GetString("default_model_ct", "");
    g_vecDefaultModels_T  = (szT  && *szT)  ? SplitString(szT,  ',') : std::vector<std::string>{};
    g_vecDefaultModels_CT = (szCT && *szCT) ? SplitString(szCT, ',') : std::vector<std::string>{};

    delete kv;
}

static void PrecacheSkins()
{
    if (!g_pPrecacher) return;

    std::set<std::string> seen;
    auto tryAdd = [&](const std::string& m) {
        if (!m.empty() && seen.insert(m).second)
            g_pPrecacher->AddPrecache(m.c_str());
    };

    for (const auto& [id, skin] : g_Skins)
    {
        for (const auto& m : skin.vecModels_T)  tryAdd(m);
        for (const auto& m : skin.vecModels_CT) tryAdd(m);
    }
    for (const auto& m : g_vecDefaultModels_T)  tryAdd(m);
    for (const auto& m : g_vecDefaultModels_CT) tryAdd(m);

    ConColorMsg(Color(0,255,0,255), "[SC] Precached %d unique models\n", (int)seen.size());
}

static void ShowSkinsMenu(int iSlot);
static void ShowAdminPlayerMenu(int iAdminSlot);
static void ShowAdminSkinMenu(int iAdminSlot);
static void ShowAdminTimeMenu(int iAdminSlot);
static void ShowAdminActionMenu(int iAdminSlot);

static void AdminTimeCallback(const char* szBack, const char* szFront, int iItem, int iAdminSlot)
{
    if (iItem >= 7 || !szBack || szBack[0] == '\0') { ShowAdminSkinMenu(iAdminSlot); return; }

    int iTargetSlot = g_AdminState[iAdminSlot].iTargetSlot;
    if (!g_pPlayers->IsConnected(iTargetSlot)) { ShowAdminPlayerMenu(iAdminSlot); return; }

    const std::string& skinId = g_AdminState[iAdminSlot].sSkinId;
    if (g_Skins.find(skinId) == g_Skins.end()) { ShowAdminSkinMenu(iAdminSlot); return; }

    int64 iSeconds = 0;
    try { iSeconds = std::stoll(szBack); } catch (...) {}

    int64  iExpire  = (iSeconds > 0) ? ((int64)std::time(nullptr) + iSeconds) : 0;
    uint64 steamid  = g_pPlayers->GetSteamID64(iTargetSlot);

    DB_GiveSkin(steamid, skinId, iExpire);
    g_Players[iTargetSlot].mSkins[skinId] = iExpire;

    const SkinEntry& skin = g_Skins[skinId];
    bool bMulti = (skin.vecModels_T.size() > 1 || skin.vecModels_CT.size() > 1);

    g_pUtils->PrintToChat(iTargetSlot, Phrase("skin_given_target"),
        skin.sName.c_str(), bMulti ? Phrase("menu_rng", " [RNG]") : "", FormatTime(iSeconds).c_str());
    g_pUtils->PrintToChat(iAdminSlot, Phrase("skin_given_admin"),
        skin.sName.c_str(), iTargetSlot, FormatTime(iSeconds).c_str());

    ShowAdminPlayerMenu(iAdminSlot);
}

static void ShowAdminTimeMenu(int iAdminSlot)
{
    if (!g_pMenus) return;
    const auto it = g_Skins.find(g_AdminState[iAdminSlot].sSkinId);
    if (it == g_Skins.end()) { ShowAdminSkinMenu(iAdminSlot); return; }

    char szTitle[128];
    g_SMAPI->Format(szTitle, sizeof(szTitle), "[ Время: %s ]", it->second.sName.c_str());

    Menu hMenu;
    g_pMenus->SetTitleMenu(hMenu, szTitle);
    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, false);
    g_pMenus->SetCallback(hMenu, AdminTimeCallback);

    g_pMenus->AddItemMenu(hMenu, "3600",   Phrase("time_menu_1h", "1 час"));
    g_pMenus->AddItemMenu(hMenu, "86400",  Phrase("time_menu_1d", "1 день"));
    g_pMenus->AddItemMenu(hMenu, "604800", Phrase("time_menu_7d", "7 дней"));
    g_pMenus->AddItemMenu(hMenu, "0",      Phrase("time_menu_perm", "Навсегда"));

    g_pMenus->DisplayPlayerMenu(hMenu, iAdminSlot, true, true);
}

static void ShowAdminActionMenu(int iAdminSlot);

static void AdminActionCallback(const char* szBack, const char* szFront, int iItem, int iAdminSlot)
{
    if (iItem >= 7 || !szBack || szBack[0] == '\0') { ShowAdminSkinMenu(iAdminSlot); return; }

    int iTargetSlot = g_AdminState[iAdminSlot].iTargetSlot;
    if (!g_pPlayers->IsConnected(iTargetSlot)) { ShowAdminPlayerMenu(iAdminSlot); return; }

    const std::string& skinId = g_AdminState[iAdminSlot].sSkinId;
    uint64 steamid = g_pPlayers->GetSteamID64(iTargetSlot);

    if (strcmp(szBack, "__give__") == 0)
    {
        ShowAdminTimeMenu(iAdminSlot);
        return;
    }

    if (strcmp(szBack, "__remove__") == 0)
    {
        DB_RemoveSkin(steamid, skinId);
        g_Players[iTargetSlot].mSkins.erase(skinId);
        if (g_Players[iTargetSlot].sSkinId == skinId)
        {
            g_Players[iTargetSlot].sSkinId = "";
            DB_SaveSelected(steamid, "");
            RestoreDefault(iTargetSlot);
            ApplyDefaultSkin(iTargetSlot);
        }
        g_pUtils->PrintToChat(iTargetSlot, Phrase("skin_removed_target"), skinId.c_str());
        g_pUtils->PrintToChat(iAdminSlot, Phrase("skin_removed_target"), skinId.c_str());
        ShowAdminSkinMenu(iAdminSlot);
        return;
    }

    if (strcmp(szBack, "__deselect__") == 0)
    {
        if (g_Players[iTargetSlot].sSkinId == skinId)
        {
            g_Players[iTargetSlot].sSkinId = "";
            DB_SaveSelected(steamid, "");
            RestoreDefault(iTargetSlot);
            ApplyDefaultSkin(iTargetSlot);
            g_pUtils->PrintToChat(iTargetSlot, Phrase("skin_reset"));
            g_pUtils->PrintToChat(iAdminSlot, Phrase("skin_reset"));
        }
        ShowAdminSkinMenu(iAdminSlot);
        return;
    }

    ShowAdminSkinMenu(iAdminSlot);
}

static void ShowAdminActionMenu(int iAdminSlot)
{
    if (!g_pMenus) return;

    int iTargetSlot = g_AdminState[iAdminSlot].iTargetSlot;
    if (!g_pPlayers->IsConnected(iTargetSlot)) { ShowAdminPlayerMenu(iAdminSlot); return; }

    const std::string& skinId = g_AdminState[iAdminSlot].sSkinId;
    auto sit = g_Skins.find(skinId);
    if (sit == g_Skins.end()) { ShowAdminSkinMenu(iAdminSlot); return; }

    int64 iNow     = (int64)std::time(nullptr);
    bool  bHasDB   = false;
    bool  bSelected = (g_Players[iTargetSlot].sSkinId == skinId);
    auto  dit      = g_Players[iTargetSlot].mSkins.find(skinId);
    if (dit != g_Players[iTargetSlot].mSkins.end())
        bHasDB = (dit->second == 0 || dit->second > iNow);

    auto pController = CCSPlayerController::FromSlot(iTargetSlot);
    const char* szName = (pController && pController->GetPlayerName()) ? pController->GetPlayerName() : "?";

    char szTitle[128];
    g_SMAPI->Format(szTitle, sizeof(szTitle), "[ %s: %s ]", sit->second.sName.c_str(), szName);

    Menu hMenu;
    g_pMenus->SetTitleMenu(hMenu, szTitle);
    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, false);
    g_pMenus->SetCallback(hMenu, AdminActionCallback);

    g_pMenus->AddItemMenu(hMenu, "__give__",     Phrase("admin_action_give", "Выдать"));
    if (bHasDB)
        g_pMenus->AddItemMenu(hMenu, "__remove__", Phrase("admin_action_remove", "Забрать (DB)"));
    if (bSelected)
        g_pMenus->AddItemMenu(hMenu, "__deselect__", Phrase("admin_action_deselect", "Выключить (снять выбор)"));

    g_pMenus->DisplayPlayerMenu(hMenu, iAdminSlot, true, true);
}

static void AdminSkinCallback(const char* szBack, const char* szFront, int iItem, int iAdminSlot)
{
    if (iItem >= 7 || !szBack || szBack[0] == '\0') { ShowAdminPlayerMenu(iAdminSlot); return; }

    int iTargetSlot = g_AdminState[iAdminSlot].iTargetSlot;

    if (strcmp(szBack, "__revoke__") == 0)
    {
        if (g_pPlayers->IsConnected(iTargetSlot))
        {
            uint64 steamid = g_pPlayers->GetSteamID64(iTargetSlot);
            DB_RevokeAll(steamid);
            DB_SaveSelected(steamid, "");
            g_Players[iTargetSlot].mSkins.clear();
            g_Players[iTargetSlot].sSkinId = "";
            RestoreDefault(iTargetSlot);
            ApplyDefaultSkin(iTargetSlot);
            g_pUtils->PrintToChat(iTargetSlot, Phrase("skins_reset_target"));
            g_pUtils->PrintToChat(iAdminSlot, Phrase("skins_reset_admin"), iTargetSlot);
        }
        ShowAdminPlayerMenu(iAdminSlot);
        return;
    }

    if (g_Skins.find(szBack) == g_Skins.end()) { ShowAdminPlayerMenu(iAdminSlot); return; }

    g_AdminState[iAdminSlot].sSkinId = szBack;
    ShowAdminActionMenu(iAdminSlot);
}

static void ShowAdminSkinMenu(int iAdminSlot)
{
    if (!g_pMenus) return;

    int iTargetSlot = g_AdminState[iAdminSlot].iTargetSlot;
    if (!g_pPlayers->IsConnected(iTargetSlot)) { ShowAdminPlayerMenu(iAdminSlot); return; }

    auto pController = CCSPlayerController::FromSlot(iTargetSlot);
    const char* szName = (pController && pController->GetPlayerName()) ? pController->GetPlayerName() : "?";

    char szTitle[128];
    g_SMAPI->Format(szTitle, sizeof(szTitle), "[ Скины: %s ]", szName);

    int64 iNow = (int64)std::time(nullptr);

    Menu hMenu;
    g_pMenus->SetTitleMenu(hMenu, szTitle);
    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, false);
    g_pMenus->SetCallback(hMenu, AdminSkinCallback);

    g_pMenus->AddItemMenu(hMenu, "__revoke__", Phrase("admin_revoke", "[ Сбросить все DB скины ]"));

    for (const auto& [id, skin] : g_Skins)
    {
        std::string sDisplay = skin.sName;
        if (skin.vecModels_T.size() > 1 || skin.vecModels_CT.size() > 1)
            sDisplay += Phrase("menu_rng", " [RNG]");
        auto dit = g_Players[iTargetSlot].mSkins.find(id);
        if (dit != g_Players[iTargetSlot].mSkins.end() && (dit->second == 0 || dit->second > iNow))
            sDisplay += Phrase("menu_has_skin", " [DB]");
        if (g_Players[iTargetSlot].sSkinId == id)
            sDisplay += " [✔]";
        g_pMenus->AddItemMenu(hMenu, id.c_str(), sDisplay.c_str());
    }

    g_pMenus->DisplayPlayerMenu(hMenu, iAdminSlot, true, true);
}

static void AdminPlayerCallback(const char* szBack, const char* szFront, int iItem, int iAdminSlot)
{
    if (iItem >= 7 || !szBack || szBack[0] == '\0') { ShowSkinsMenu(iAdminSlot); return; }

    if (szBack[0] == 'p' && szBack[1] == '_')
    {
        int iTargetSlot = -1;
        try { iTargetSlot = std::stoi(szBack + 2); } catch (...) {}
        if (iTargetSlot >= 0 && iTargetSlot < 64 && g_pPlayers->IsConnected(iTargetSlot))
        {
            g_AdminState[iAdminSlot].iTargetSlot = iTargetSlot;
            g_AdminState[iAdminSlot].sSkinId     = "";
            ShowAdminSkinMenu(iAdminSlot);
            return;
        }
    }
    ShowAdminPlayerMenu(iAdminSlot);
}

static void ShowAdminPlayerMenu(int iAdminSlot)
{
    if (!g_pMenus) return;

    Menu hMenu;
    g_pMenus->SetTitleMenu(hMenu, Phrase("admin_menu_title", "[ Управление - Игрок ]"));
    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, false);
    g_pMenus->SetCallback(hMenu, AdminPlayerCallback);

    bool bAny = false;
    for (int i = 0; i < 64; i++)
    {
        if (!g_pPlayers->IsConnected(i) || g_pPlayers->IsFakeClient(i)) continue;
        auto pController = CCSPlayerController::FromSlot(i);
        if (!pController) continue;

        const char* szName = pController->GetPlayerName();
        if (!szName || !szName[0]) szName = "Unknown";

        char szKey[16];
        g_SMAPI->Format(szKey, sizeof(szKey), "p_%d", i);

        std::string sDisplay = szName;
        if (!g_Players[i].mSkins.empty()) sDisplay += Phrase("menu_db_mark", " [DB]");

        g_pMenus->AddItemMenu(hMenu, szKey, sDisplay.c_str());
        bAny = true;
    }

    if (!bAny) g_pMenus->AddItemMenu(hMenu, "__empty__", Phrase("admin_no_players", "Нет игроков онлайн"));

    g_pMenus->DisplayPlayerMenu(hMenu, iAdminSlot, true, true);
}

static void MenuCallback(const char* szBack, const char* szFront, int iItem, int iSlot)
{
    if (iItem >= 7 || !szBack || szBack[0] == '\0') { return; }

    if (strcmp(szBack, "__manage__") == 0)
    {
        g_AdminState[iSlot] = {};
        ShowAdminPlayerMenu(iSlot);
        return;
    }

    if (strcmp(szBack, "__empty__") == 0) { ShowSkinsMenu(iSlot); return; }

    if (g_Players[iSlot].iCooldownEnd > (int)std::time(nullptr))
    {
        g_pUtils->PrintToChat(iSlot, Phrase("cooldown"),
            g_Players[iSlot].iCooldownEnd - (int)std::time(nullptr));
        ShowSkinsMenu(iSlot);
        return;
    }

    g_Players[iSlot].iCooldownEnd = (int)std::time(nullptr) + g_iCooldown;
    uint64 steamid = g_pPlayers->GetSteamID64(iSlot);

    if (strcmp(szBack, "__none__") == 0)
    {
        g_Players[iSlot].sSkinId = "";
        DB_SaveSelected(steamid, "");
        RestoreDefault(iSlot);
        ApplyDefaultSkin(iSlot);
        g_pUtils->PrintToChat(iSlot, Phrase("skin_reset"));
    }
    else
    {
        if (!PlayerCanAccessSkin(iSlot, szBack)) { return; }

        g_Players[iSlot].sSkinId = szBack;
        DB_SaveSelected(steamid, szBack);

        auto pC = CCSPlayerController::FromSlot(iSlot);
        if (pC) { auto pP = pC->GetPlayerPawn(); if (pP && pP->IsAlive()) ApplySkinModel(iSlot, szBack); }

        const SkinEntry& skin = g_Skins[szBack];
        bool bMulti = (skin.vecModels_T.size() > 1 || skin.vecModels_CT.size() > 1);
        g_pUtils->PrintToChat(iSlot, Phrase("skin_selected"),
            skin.sName.c_str(), bMulti ? Phrase("menu_rng", " [RNG]") : "");
    }
    ShowSkinsMenu(iSlot);
}

static void ShowSkinsMenu(int iSlot)
{
    if (!g_pMenus) return;

    bool bAdmin = IsAdmin(iSlot);

    if (!bAdmin && !PlayerHasAnyAccessibleSkin(iSlot))
    {
        g_pUtils->PrintToChat(iSlot, Phrase("no_access"));
        return;
    }

    const std::string& cur  = g_Players[iSlot].sSkinId;
    int64              iNow = (int64)std::time(nullptr);

    Menu hMenu;
    g_pMenus->SetTitleMenu(hMenu, Phrase("menu_title", "[ Skins ]"));
    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, false);
    g_pMenus->SetCallback(hMenu, MenuCallback);

    if (bAdmin)
        g_pMenus->AddItemMenu(hMenu, "__manage__", Phrase("menu_manage", "[ Управление ]"));

    if (PlayerHasAnyAccessibleSkin(iSlot))
    {
        g_pMenus->AddItemMenu(hMenu, "__none__", cur.empty() ? Phrase("menu_default_selected", "[✔] Стандартный") : Phrase("menu_default", "Стандартный"));

        for (const auto& [id, skin] : g_Skins)
        {
            if (!PlayerCanAccessSkin(iSlot, id)) continue;

            std::string sDisplay = skin.sName;
            if (skin.vecModels_T.size() > 1 || skin.vecModels_CT.size() > 1)
                sDisplay += Phrase("menu_rng", " [RNG]");

            auto dit = g_Players[iSlot].mSkins.find(id);
            if (dit != g_Players[iSlot].mSkins.end() && dit->second > 0)
            {
                int64 iLeft = dit->second - iNow;
                if (iLeft > 0)
                {
                    char buf[32];
                    g_SMAPI->Format(buf, sizeof(buf), " [%s]", FormatTime(iLeft).c_str());
                    sDisplay += buf;
                }
            }

            if (cur == id) sDisplay = "[✔] " + sDisplay;
            g_pMenus->AddItemMenu(hMenu, id.c_str(), sDisplay.c_str());
        }
    }

    g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

static void OnPlayerSpawn(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
    int iSlot = pEvent->GetInt("userid");
    if (iSlot < 0 || iSlot >= 64) return;

    g_Players[iSlot].sDefaultModel[2] = "";
    g_Players[iSlot].sDefaultModel[3] = "";


    if (g_pPlayers->IsFakeClient(iSlot)) return;

    if (g_fApplyDelay <= 0.0f) { ApplySkinOnSpawn(iSlot); return; }

    g_pUtils->CreateTimer(g_fApplyDelay, [iSlot]() -> float {
        if (g_pPlayers->IsConnected(iSlot)) ApplySkinOnSpawn(iSlot);
        return -1.0f;
    });
}

static void OnPlayerTeam(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
    int iSlot = pEvent->GetInt("userid");
    if (iSlot < 0 || iSlot >= 64) return;
    g_Players[iSlot].sDefaultModel[2] = "";
    g_Players[iSlot].sDefaultModel[3] = "";
}

static void OnPlayerConnectFull(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
    int iSlot = pEvent->GetInt("userid");
    if (iSlot < 0 || iSlot >= 64) return;
    if (g_pPlayers->IsFakeClient(iSlot)) return;

    g_Players[iSlot] = {};

    uint64 steamid = g_pPlayers->GetSteamID64(iSlot);
    if (steamid) DB_Load(iSlot, steamid);
}

static void OnPlayerDisconnect(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
    int iSlot = pEvent->GetInt("userid");
    if (iSlot < 0 || iSlot >= 64) return;
    g_Players[iSlot] = {};
}

static bool CMD_SkinMenu(int iSlot, const char* szContent)
{
    if (!g_pPlayers->IsConnected(iSlot) || g_pPlayers->IsFakeClient(iSlot)) return false;
    ShowSkinsMenu(iSlot);
    return true;
}

CON_COMMAND_F(mm_giveskin,
    "SkinsCore: mm_giveskin <steamid64> <skin_id> [seconds]",
    FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE)
{
    if (args.ArgC() < 3) { Msg("[SC] Usage: mm_giveskin <steamid64> <skin_id> [seconds]\n"); return; }

    const char* szSkin = args[2];
    if (g_Skins.find(szSkin) == g_Skins.end()) { Msg("[SC] Unknown skin '%s'\n", szSkin); return; }

    uint64 steamid  = std::stoull(args[1]);
    int64  iSeconds = (args.ArgC() >= 4) ? std::stoll(args[3]) : 0;
    int64  iExpire  = (iSeconds > 0) ? ((int64)std::time(nullptr) + iSeconds) : 0;

    DB_GiveSkin(steamid, szSkin, iExpire);

    for (int i = 0; i < 64; i++)
    {
        if (!g_pPlayers->IsConnected(i) || g_pPlayers->IsFakeClient(i)) continue;
        if (g_pPlayers->GetSteamID64(i) != steamid) continue;
        g_Players[i].mSkins[szSkin] = iExpire;
        const SkinEntry& skin = g_Skins[szSkin];
        bool bMulti = (skin.vecModels_T.size() > 1 || skin.vecModels_CT.size() > 1);
        g_pUtils->PrintToChat(i, Phrase("skin_given_target"),
            skin.sName.c_str(), bMulti ? Phrase("menu_rng", " [RNG]") : "", FormatTime(iSeconds).c_str());
        break;
    }
    Msg("[SC] Gave '%s' to %s for %s\n", szSkin, args[1], FormatTime(iSeconds).c_str());
}

CON_COMMAND_F(mm_removeskin,
    "SkinsCore: mm_removeskin <steamid64> <skin_id>",
    FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE)
{
    if (args.ArgC() < 3) { Msg("[SC] Usage: mm_removeskin <steamid64> <skin_id>\n"); return; }

    uint64      steamid = std::stoull(args[1]);
    std::string skinId  = args[2];

    DB_RemoveSkin(steamid, skinId);

    for (int i = 0; i < 64; i++)
    {
        if (!g_pPlayers->IsConnected(i) || g_pPlayers->IsFakeClient(i)) continue;
        if (g_pPlayers->GetSteamID64(i) != steamid) continue;
        g_Players[i].mSkins.erase(skinId);
        if (g_Players[i].sSkinId == skinId)
        {
            g_Players[i].sSkinId = "";
            DB_SaveSelected(steamid, "");
            RestoreDefault(i);
            ApplyDefaultSkin(i);
        }
        g_pUtils->PrintToChat(i, Phrase("skin_removed_target"), skinId.c_str());
        break;
    }
    Msg("[SC] Removed skin '%s' from %s\n", skinId.c_str(), args[1]);
}

CON_COMMAND_F(mm_revokeaccess,
    "SkinsCore: mm_revokeaccess <steamid64>",
    FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE)
{
    if (args.ArgC() < 2) { Msg("[SC] Usage: mm_revokeaccess <steamid64>\n"); return; }

    uint64 steamid = std::stoull(args[1]);
    DB_RevokeAll(steamid);
    DB_SaveSelected(steamid, "");

    for (int i = 0; i < 64; i++)
    {
        if (!g_pPlayers->IsConnected(i) || g_pPlayers->IsFakeClient(i)) continue;
        if (g_pPlayers->GetSteamID64(i) != steamid) continue;
        g_Players[i].mSkins.clear();
        g_Players[i].sSkinId = "";
        RestoreDefault(i);
        ApplyDefaultSkin(i);
        g_pUtils->PrintToChat(i, Phrase("access_revoked"));
        break;
    }
    Msg("[SC] Revoked all skins from %s\n", args[1]);
}

CON_COMMAND_F(mm_listskinscore,
    "SkinsCore: list all skin IDs",
    FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE)
{
    Msg("[SC] Skins (%d):\n", (int)g_Skins.size());
    for (const auto& [id, skin] : g_Skins)
        Msg("  %-28s  %-22s  T:%-2d  CT:%d  VIP:%d  Admin:%d  Personal:%d\n",
            id.c_str(), skin.sName.c_str(),
            (int)skin.vecModels_T.size(), (int)skin.vecModels_CT.size(),
            (int)skin.vecVIPGroups.size(), (int)skin.vecAdminFlags.size(),
            (int)skin.vecSteamIDs.size());
}

CON_COMMAND_F(mm_reloadskins,
    "SkinsCore: reload config and skins without restart",
    FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE)
{
    LoadConfig();
    LoadSkins();
    PrecacheSkins();
    LoadPhrases();
    Msg("[SC] Reloaded: %d skins\n", (int)g_Skins.size());
}

static void OnStartupServer()
{
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem     = g_pUtils->GetCEntitySystem();
    LoadDBCreds();
    DB_Connect();
    PrecacheSkins();
    LoadPhrases();
}

bool SkinsCore::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);

    ConVar_Register(FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE);
    g_SMAPI->AddListener(this, this);
    for (int i = 0; i < 64; i++) g_Players[i] = {};
    LoadConfig();
    LoadSkins();
    return true;
}

bool SkinsCore::Unload(char* error, size_t maxlen)
{
    if (g_pUtils) g_pUtils->ClearAllHooks(g_PLID);
    if (g_pDB)    { g_pDB->Destroy(); g_pDB = nullptr; }
    ConVar_Unregister();
    return true;
}

void SkinsCore::AllPluginsLoaded()
{
    int ret;

    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED)
    {
        ConColorMsg(Color(255,0,0,255), "[SC] Utils not found\n");
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str()); return;
    }
    g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED)
    {
        ConColorMsg(Color(255,0,0,255), "[SC] Players not found\n");
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str()); return;
    }
    g_pMenus = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED)
    {
        ConColorMsg(Color(255,0,0,255), "[SC] Menus not found\n");
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str()); return;
    }

    g_pPrecacher = (IResourcePrecacher*)g_SMAPI->MetaFactory(RESOURCE_PRECACHER_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) g_pPrecacher = nullptr;

    g_pVIP = (IVIPApi*)g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) g_pVIP = nullptr;

    g_pAdmin = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) g_pAdmin = nullptr;

    g_pUtils->StartupServer(g_PLID, OnStartupServer);
    g_pUtils->HookEvent(g_PLID, "player_spawn",        OnPlayerSpawn);
    g_pUtils->HookEvent(g_PLID, "player_team",         OnPlayerTeam);
    g_pUtils->HookEvent(g_PLID, "player_connect_full", OnPlayerConnectFull);
    g_pUtils->HookEvent(g_PLID, "player_disconnect",   OnPlayerDisconnect);

    if (g_vecChatCmds.empty()) g_vecChatCmds = {"!skin", "!skins"};
    g_pUtils->RegCommand(g_PLID, {}, g_vecChatCmds, CMD_SkinMenu);
}

///////////////////////////////////////
const char* SkinsCore::GetLicense()
{
	return "Public";
}

const char* SkinsCore::GetVersion()
{
	return "1.1";
}

const char* SkinsCore::GetDate()
{
	return __DATE__;
}

const char *SkinsCore::GetLogTag()
{
	return "[SC]";
}

const char* SkinsCore::GetAuthor()
{
	return "_ded_cookies";
}

const char* SkinsCore::GetDescription()
{
	return "Skins Core";
}

const char* SkinsCore::GetName()
{
	return "Skins Core";
}

const char* SkinsCore::GetURL()
{
	return "https://api.onlypublic.net/";
}
