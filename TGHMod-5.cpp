// TGHMod  (C++ ASI, 基于 Aru 的 GTA IV ScriptHook SDK)   —— v6（排查闪退版）
//
// v6 改动：T 键走的新功能全部做成"运行时开关"，由游戏目录里的 TGHMod.ini 控制，
//          不用重新编译就能逐项打开，找出是哪一项导致 T 闪退。
//          没有 TGHMod.ini 时，所有高风险功能默认关闭（行为接近旧版）。
//          日志（TGHMod.log）里会写明读到的开关值，并在每一步之前先写一行。
//
// 按键：
//   T  灵魂出窍：Niko 原地冻结，镜头脱离，自由飞行
//   G  附身：镜头对准的 NPC 倒地抽搐 1.8 秒，然后你"变成"他（真正的玩家控制）
//   H  返回 Niko
//
// 灵魂状态：
//   W/S 沿镜头朝向前进/后退（往下看再按 W 就会下降）  A/D 左右平移
//   上升：Space 或 PageUp    下降：C 或 PageDown    Shift 加速
//   转向：Q/E 或 ←/→         俯仰：R/F 或 ↑/↓
//   鼠标 / 右摇杆：直接转视角
// 附身状态：完全就是正常玩游戏（走、跑、攻击、上车都可以），按 H 变回 Niko
//
// v5 改动：
//   - T：Niko 原地留下一具尸体，本体隐身；G 选人带准星，锁定目标时准星变绿
//   - G：抽搐更猛；H 退出时被附身的 NPC 倒地死亡变尸体，Niko 回到按 T 时的位置
// v3 改动：
//   - 灵魂模式支持鼠标 / 右摇杆转视角
//   - G 的选人改为沿视线扫描，近距离也能选中
//   - 换回 Niko 时恢复他的武器和护甲
// v2 改动：
//   1. 灵魂模式新增垂直移动，并且 W/S 沿三维视线方向移动
//   2. 附身改为"把玩家模型换成目标 NPC 的模型 + 外观，并删除原 NPC"，
//      所以不会再有"NPC 自己乱走、你什么都做不了"的问题
// 运行时会在游戏目录写 TGHMod.log，出问题把它发给我。

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#include "ScriptHook/ScriptHookManager.h"
#include "ScriptHook/NativeFiberThread.h"
#include "ScriptHook/Scripting.h"

using namespace Scripting;                 // ScriptingDirty.h 里直接用了 ScriptAny，必须先有这一行

#include "ScriptHook/ScriptingDirty.h"

// ---------------- 可调参数 ----------------
static const u32 CONVULSE_MS    = 1800;   // 抽搐时长
static const f32 SHAKE_FORCE    = 0.12f;  // 轻微水平扰动，避免把 NPC 打飞
static const f32 SHAKE_LIFT     = 0.015f; // 极小垂直扰动，避免腾空
static const f32 SHAKE_SPIN     = 0.10f;  // 轻微随机扭转，模拟抽搐
static const f32 CAM_JITTER     = 0.045f; // 抽搐时镜头轻微抖动
static const u32 SHAKE_EVERY_MS = 110;    // 施力间隔
static const f32 SPIRIT_SPEED   = 0.35f;  // 灵魂镜头每帧移动距离
static const f32 PICK_RADIUS    = 5.0f;   // 选目标：镜头视线落点附近多少米内找人
static const f32 FOLLOW_DIST    = 5.0f;   // 附身后镜头距离
static const f32 FOLLOW_HEIGHT  = 2.2f;   // 附身后镜头高度
static const f32 MOUSE_SENS     = 0.0035f; // 鼠标灵敏度（弧度/像素）
static const f32 STICK_YAW      = 0.05f;   // 右摇杆左右转速（每帧最大弧度）
static const f32 STICK_PITCH    = 0.04f;   // 右摇杆上下转速
static const int STICK_DEADZONE = 24;      // 摇杆死区（0~127）
static const bool STICK_INVERT_Y = false;  // 右摇杆上下方向反了就改成 true
static const f32 SCAN_MAX       = 45.0f;   // G 选人：沿视线最远扫描距离
static const f32 SCAN_STEP      = 1.0f;    // G 选人：扫描步长
// （MAX_WEAPON_ID / ENABLE_* 已改为 TGHMod.ini 里的运行时开关，见下面 g_* 变量）
static const int NUM_WEAPON_SLOTS = 50;    // 武器缓存数组大小（>= MAX_WEAPON_ID）
static const int  CORPSE_PED_TYPE = 4;      // CREATE_CHAR 的人物类型（4 = 平民男性）
static const int NUM_COMP       = 11;     // 复制外观时处理的部件数（head..face）

static const f32 PI_F = 3.14159265f;

// ---------------- 运行时开关（TGHMod.ini） ----------------
static bool g_appearance = false;   // 保存/复制外观部件
static bool g_corpse     = false;   // T 时在原地生成 Niko 尸体
static bool g_weapons    = false;   // 保存/恢复武器和护甲
static bool g_hide       = false;   // T 时让 Niko 隐身并无敌
static bool g_mouse      = false;   // 鼠标转视角
static bool g_stick      = false;   // 右摇杆转视角
static bool g_crosshair  = false;   // 画准星
static bool g_scan       = false;   // 灵魂模式下每 150ms 自动扫描目标
static bool g_verbose    = true;    // 更详细的日志
static int  g_maxWeapon  = 40;      // 保存武器时检查的武器编号上限

// ---------------- 日志 ----------------
static void LogMsg(const char *fmt, ...)
{
    FILE *f = fopen("TGHMod.log", "a");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)                       // 新文件：写入 UTF-8 BOM，记事本才不会显示成乱码
        fwrite("\xEF\xBB\xBF", 1, 3, f);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static void LoadIni()
{
    const char *f = ".\\TGHMod.ini";
    g_appearance = GetPrivateProfileIntA("TGHMod", "Appearance", 0, f) != 0;
    g_corpse     = GetPrivateProfileIntA("TGHMod", "Corpse",     0, f) != 0;
    g_weapons    = GetPrivateProfileIntA("TGHMod", "Weapons",    0, f) != 0;
    g_hide       = GetPrivateProfileIntA("TGHMod", "HideNiko",   0, f) != 0;
    g_mouse      = GetPrivateProfileIntA("TGHMod", "MouseLook",  1, f) != 0;
    g_stick      = GetPrivateProfileIntA("TGHMod", "StickLook",  0, f) != 0;
    g_crosshair  = GetPrivateProfileIntA("TGHMod", "Crosshair",  0, f) != 0;
    g_scan       = GetPrivateProfileIntA("TGHMod", "AutoScan",   0, f) != 0;
    g_verbose    = GetPrivateProfileIntA("TGHMod", "Verbose",    1, f) != 0;
    g_maxWeapon  = (int)GetPrivateProfileIntA("TGHMod", "MaxWeaponId", 40, f);
    if (g_maxWeapon < 1) g_maxWeapon = 1;
    if (g_maxWeapon > NUM_WEAPON_SLOTS - 1) g_maxWeapon = NUM_WEAPON_SLOTS - 1;
    LogMsg("INI: Appearance=%d Corpse=%d Weapons=%d HideNiko=%d MouseLook=%d StickLook=%d Crosshair=%d AutoScan=%d Verbose=%d MaxWeaponId=%d",
           (int)g_appearance, (int)g_corpse, (int)g_weapons, (int)g_hide, (int)g_mouse, (int)g_stick,
           (int)g_crosshair, (int)g_scan, (int)g_verbose, g_maxWeapon);
}

// 屏幕左下角提示
static void ShowText(const char *text, u32 ms)
{
    PrintStringWithLiteralStringNow("STRING", text, ms, 1);
}

// ---------------- 输入 ----------------
static bool GameFocused()
{
    HWND h = GetForegroundWindow();
    if (!h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

static bool KeyDown(int vk)
{
    return GameFocused() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// 只在按下的那一帧返回 true（每个键每帧只调用一次）
static bool Pressed(int vk)
{
    static bool prev[256] = { false };
    bool now = KeyDown(vk);
    bool r = now && !prev[vk & 255];
    prev[vk & 255] = now;
    return r;
}

// ---------------- 状态 ----------------
enum Mode { MODE_IDLE, MODE_SPIRIT, MODE_CONVULSE, MODE_POSSESSED };

class TGHThread : public NativeFiberThread
{
private:
    Mode   m_mode;
    Player m_player;
    Ped    m_niko;
    Ped    m_target;
    Camera m_cam;
    bool   m_camOn;

    f32 m_camX, m_camY, m_camZ;
    f32 m_yaw, m_pitch;     // 弧度，yaw=0 朝北(+Y)
    f32 m_groundZ;

    u32 m_convulseEnd;
    u32 m_nextShake;
    u32 m_rng;

    eModel m_nikoModel;          // Niko 原本的模型
    int    m_nikoDV[NUM_COMP];   // Niko 原本的外观部件
    int    m_nikoTV[NUM_COMP];
    bool   m_needRestore;        // 附身时死亡 -> 复活后要把模型换回 Niko

    bool   m_mouseInit;
    int    m_lastX, m_lastY;
    bool   m_firstLook;
    int    m_wList[NUM_WEAPON_SLOTS];   // Niko 的武器和弹药
    u32    m_wAmmo[NUM_WEAPON_SLOTS];
    int    m_wCount;
    eWeapon m_wCur;
    u32    m_nikoArmour;

    Ped    m_nikoCorpse;                 // 按 T 时留下的 Niko 尸体
    f32    m_cX, m_cY, m_cZ, m_cH;       // Niko 尸体位置（H 退出时回到这里）
    eModel m_possModel;                  // 被附身 NPC 的模型 / 外观（H 时用来生成尸体）
    int    m_possDV[NUM_COMP];
    int    m_possTV[NUM_COMP];
    Ped    m_hint;                       // 当前准星锁定的人
    bool   m_firstSpirit;                // 灵魂模式第一帧（用于逐步日志）
    u32    m_nextScan;

    f32 Rand01()
    {
        m_rng = m_rng * 1664525u + 1013904223u;
        return (f32)((m_rng >> 8) & 0xFFFF) / 65535.0f;
    }

    bool Alive(Ped p)
    {
        return p != 0 && DoesCharExist(p) && !IsCharDead(p);
    }

    // ---- 外观 / 模型 ----
    void SaveComponents(Ped p, int *dv, int *tv)
    {
        if (!g_appearance) { LogMsg("SaveComponents: 已关闭(Appearance=0)，跳过"); return; }
        for (int c = 0; c < NUM_COMP; c++)
        {
            if (g_verbose) LogMsg("SaveComponents: 部件 %d", c);
            dv[c] = (int)GetCharDrawableVariation(p, c);
            tv[c] = (int)GetCharTextureVariation(p, c);
        }
    }

    void ApplyComponents(Ped p, const int *dv, const int *tv)
    {
        if (!g_appearance) return;
        for (int c = 0; c < NUM_COMP; c++)
            SetCharComponentVariation(p, c, (u32)dv[c], (u32)tv[c]);
    }

    bool LoadModelWait(eModel model)
    {
        RequestModel(model);
        u32 start = GetTickCount();
        while (!HasModelLoaded(model))
        {
            LoadAllObjectsNow();
            if (GetTickCount() - start > 3000)
            {
                LogMsg("LoadModelWait: 模型 0x%08X 加载超时", (unsigned)model);
                return false;
            }
            Wait(0);
        }
        return true;
    }

    // 在 (x,y,z) 生成一具尸体（用指定模型和外观）。persistent=true 时不会被游戏自动清理
    bool SpawnCorpse(eModel model, f32 x, f32 y, f32 z, f32 h, const int *dv, const int *tv,
                     bool persistent, Ped *out)
    {
        *out = 0;
        if (model == 0) { LogMsg("SpawnCorpse: 模型无效，跳过"); return false; }
        LogMsg("SpawnCorpse: 加载模型 0x%08X", (unsigned)model);
        if (!LoadModelWait(model)) return false;

        Ped c = 0;
        LogMsg("SpawnCorpse: 创建");
        CreateChar(CORPSE_PED_TYPE, model, x, y, z, &c, true);
        if (c == 0 || !DoesCharExist(c))
        {
            LogMsg("SpawnCorpse: CreateChar 失败 model=0x%08X", (unsigned)model);
            return false;
        }
        ApplyComponents(c, dv, tv);
        SetCharHeading(c, h);
        SetCharInvincible(c, false);
        SetCharHealth(c, 0);
        if (!IsCharDead(c)) DamageChar(c, 9999, true);

        LogMsg("SpawnCorpse: 完成");
        *out = c;
        if (!persistent) { Ped tmp = c; MarkCharAsNoLongerNeeded(&tmp); }
        return true;
    }

    void DeleteNikoCorpse()
    {
        if (m_nikoCorpse != 0 && DoesCharExist(m_nikoCorpse))
            DeleteChar(&m_nikoCorpse);
        m_nikoCorpse = 0;
    }

    // 把玩家模型换成 model；成功返回 true。必须在脚本线程里调用（里面会 Wait）
    bool SwapPlayerModel(eModel model)
    {
        RequestModel(model);
        u32 start = GetTickCount();
        while (!HasModelLoaded(model))
        {
            LoadAllObjectsNow();
            if (GetTickCount() - start > 3000)
            {
                LogMsg("SwapPlayerModel: 模型 0x%08X 加载超时", (unsigned)model);
                return false;
            }
            Wait(0);
        }
        ChangePlayerModel(m_player, model);
        Ped p = 0;
        GetPlayerChar(m_player, &p);     // 句柄可能变化，重新取
        if (p != 0) m_niko = p;
        return true;
    }

    // ---- 武器 / 护甲：换模型会被清空，所以先存后还 ----
    void SaveWeapons()
    {
        m_wCount = 0;
        m_wCur = 0;
        m_nikoArmour = 0;
        if (!g_weapons) { LogMsg("SaveWeapons: 已关闭(Weapons=0)，跳过"); return; }

        for (int w = 1; w <= g_maxWeapon; w++)
        {
            if (g_verbose) LogMsg("SaveWeapons: 检查武器 %d", w);
            if (!HasCharGotWeapon(m_niko, w)) continue;
            u32 ammo = 0;
            GetAmmoInCharWeapon(m_niko, w, &ammo);
            m_wList[m_wCount] = w;
            m_wAmmo[m_wCount] = ammo;
            m_wCount++;
        }
        GetCurrentCharWeapon(m_niko, &m_wCur);
        GetCharArmour(m_niko, &m_nikoArmour);
        LogMsg("保存武器 %d 把，护甲 %u", m_wCount, (unsigned)m_nikoArmour);
    }

    void RestoreWeapons()
    {
        if (!g_weapons) return;
        for (int i = 0; i < m_wCount; i++)
            GiveWeaponToChar(m_niko, m_wList[i], m_wAmmo[i], false);
        if (m_wCur != 0 && m_wCount > 0)
            SetCurrentCharWeapon(m_niko, m_wCur, true);
        u32 arm = 0;
        GetCharArmour(m_niko, &arm);
        if (m_nikoArmour > arm) AddArmourToChar(m_niko, m_nikoArmour - arm);
        LogMsg("恢复武器 %d 把", m_wCount);
    }

    // ---- 鼠标 / 右摇杆转视角（灵魂模式） ----
    void LookInput()
    {
        if (m_firstLook) LogMsg("LookInput 首帧开始");

        // 盖世游戏的鼠标更适合“中心锁定 + 每帧取偏移”。
        // 不等到窗口边缘才回中，否则虚拟鼠标很容易看起来完全不动。
        if (g_mouse)
        {
            HWND h = GetForegroundWindow();
            if (h && GameFocused())
            {
                RECT rc;
                POINT center;
                if (GetClientRect(h, &rc))
                {
                    POINT origin;
                    origin.x = 0; origin.y = 0;
                    if (ClientToScreen(h, &origin))
                    {
                        center.x = origin.x + (rc.right - rc.left) / 2;
                        center.y = origin.y + (rc.bottom - rc.top) / 2;

                        POINT p;
                        if (GetCursorPos(&p))
                        {
                            if (!m_mouseInit)
                            {
                                // 第一次只归中，不把进入灵魂状态前的鼠标位置算成转向。
                                SetCursorPos(center.x, center.y);
                                m_lastX = center.x;
                                m_lastY = center.y;
                                m_mouseInit = true;
                            }
                            else
                            {
                                int dx = p.x - center.x;
                                int dy = p.y - center.y;

                                // 防止盖世/窗口切换瞬间产生一个巨大的跳变。
                                if (dx > -500 && dx < 500 && dy > -500 && dy < 500)
                                {
                                    m_yaw   -= (f32)dx * MOUSE_SENS;
                                    m_pitch -= (f32)dy * MOUSE_SENS;
                                }

                                SetCursorPos(center.x, center.y);
                                m_lastX = center.x;
                                m_lastY = center.y;
                            }
                        }
                    }
                }
            }
            else
            {
                m_mouseInit = false;
            }
        }

        if (g_stick)
        {
            u32 lx = 0, ly = 0, rx = 0, ry = 0;
            GetPositionOfAnalogueSticks(0, &lx, &ly, &rx, &ry);
            int sx = (int)(i32)rx, sy = (int)(i32)ry;

            static int s_logged = 0;
            if ((sx > STICK_DEADZONE || sx < -STICK_DEADZONE ||
                 sy > STICK_DEADZONE || sy < -STICK_DEADZONE) && s_logged < 5)
            {
                LogMsg("右摇杆读数 x=%d y=%d", sx, sy);
                s_logged++;
            }

            if (sx >= -127 && sx <= 127 && sy >= -127 && sy <= 127)
            {
                if (sx > STICK_DEADZONE || sx < -STICK_DEADZONE)
                    m_yaw -= (sx / 127.0f) * STICK_YAW;

                if (sy > STICK_DEADZONE || sy < -STICK_DEADZONE)
                {
                    f32 k = (sy / 127.0f) * STICK_PITCH;
                    m_pitch += STICK_INVERT_Y ? k : -k;
                }
            }
        }

        if (m_pitch > 1.4f) m_pitch = 1.4f;
        if (m_pitch < -1.4f) m_pitch = -1.4f;

        if (m_firstLook) LogMsg("LookInput 完成");
    }

    // 把玩家换回 Niko 的模型和外观
    void RestoreNikoModel(bool withWeapons)
    {
        m_player = ConvertIntToPlayerIndex(GetPlayerId());
        if (SwapPlayerModel(m_nikoModel))
        {
            ApplyComponents(m_niko, m_nikoDV, m_nikoTV);
            if (withWeapons) RestoreWeapons();
        }
        else
            LogMsg("RestoreNikoModel: 换回 Niko 失败");
        SetPlayerControl(m_player, true);
    }

    // ---- 镜头 ----
    void CamStart()
    {
        if (m_camOn) return;
        CreateCam(14, &m_cam);
        SetCamPos(m_cam, m_camX, m_camY, m_camZ);
        CamLookAhead();
        ScriptingDirty::ActivateScriptedCams(1, 1);
        SetCamActive(m_cam, true);
        SetCamPropagate(m_cam, true);
        m_camOn = true;
    }

    void CamStop()
    {
        if (!m_camOn) return;
        SetCamActive(m_cam, false);
        SetCamPropagate(m_cam, false);
        ScriptingDirty::ActivateScriptedCams(0, 0);
        DestroyCam(m_cam);
        m_camOn = false;
    }

    void CamDir(f32 *dx, f32 *dy, f32 *dz)
    {
        f32 cp = cosf(m_pitch);
        *dx = -sinf(m_yaw) * cp;
        *dy = cosf(m_yaw) * cp;
        *dz = sinf(m_pitch);
    }

    // 镜头看向自己前方（自由飞行用）
    void CamLookAhead()
    {
        f32 dx, dy, dz;
        CamDir(&dx, &dy, &dz);
        NativeInvoke::Invoke<ScriptVoid>("POINT_CAM_AT_COORD", m_cam,
            m_camX + dx * 10.0f, m_camY + dy * 10.0f, m_camZ + dz * 10.0f);
    }

    // 镜头跟随目标（附身用）
    void CamFollow(Ped p, f32 jitter)
    {
        f32 x, y, z;
        GetCharCoordinates(p, &x, &y, &z);
        f32 fx = -sinf(m_yaw), fy = cosf(m_yaw);
        m_camX = x - fx * FOLLOW_DIST;
        m_camY = y - fy * FOLLOW_DIST;
        m_camZ = z + FOLLOW_HEIGHT;
        if (jitter > 0.0f)
        {
            m_camX += (Rand01() - 0.5f) * 2.0f * jitter;
            m_camY += (Rand01() - 0.5f) * 2.0f * jitter;
            m_camZ += (Rand01() - 0.5f) * 2.0f * jitter;
        }
        SetCamPos(m_cam, m_camX, m_camY, m_camZ);
        NativeInvoke::Invoke<ScriptVoid>("POINT_CAM_AT_COORD", m_cam, x, y, z + 0.8f);
    }

    void RotateByKeys(bool allowPitch)
    {
        const f32 rot = 0.04f;
        if (KeyDown('Q') || KeyDown(VK_LEFT))  m_yaw += rot;
        if (KeyDown('E') || KeyDown(VK_RIGHT)) m_yaw -= rot;
        if (allowPitch)
        {
            if (KeyDown('R') || KeyDown(VK_UP))   m_pitch += rot;
            if (KeyDown('F') || KeyDown(VK_DOWN)) m_pitch -= rot;
            if (m_pitch > 1.4f) m_pitch = 1.4f;
            if (m_pitch < -1.4f) m_pitch = -1.4f;
        }
    }

    // ---- T：灵魂出窍 ----
    void SpiritOn()
    {
        if (m_mode != MODE_IDLE) return;

        m_player = ConvertIntToPlayerIndex(GetPlayerId());
        m_niko = 0;
        GetPlayerChar(m_player, &m_niko);
        if (!Alive(m_niko))
        {
            LogMsg("SpiritOn: 没有取到有效的 Niko");
            return;
        }

        if (IsCharInAnyCar(m_niko))
        {
            ShowText("TGH: get out of the vehicle first.", 2500);
            return;
        }

        f32 x, y, z, h = 0.0f;
        GetCharCoordinates(m_niko, &x, &y, &z);
        GetCharHeading(m_niko, &h);

        LogMsg("SpiritOn: 开始保存 Niko 状态");
        GetCharModel(m_niko, &m_nikoModel);
        SaveComponents(m_niko, m_nikoDV, m_nikoTV);
        LogMsg("SpiritOn: 外观已保存");
        SaveWeapons();
        LogMsg("SpiritOn: 武器已保存");
        m_mouseInit = false;
        m_firstLook = true;

        m_yaw = h * PI_F / 180.0f;
        m_pitch = -0.35f;
        m_camX = x; m_camY = y; m_camZ = z + 3.0f;
        m_groundZ = z - 1.0f;

        // Niko 原地留下一具尸体，本体隐身并保持无敌
        m_cX = x; m_cY = y; m_cZ = z; m_cH = h;
        m_firstSpirit = true;
        if (g_corpse)
        {
            LogMsg("SpiritOn: 步骤 尸体 开始");
            DeleteNikoCorpse();
            if (!SpawnCorpse(m_nikoModel, x, y, z, h, m_nikoDV, m_nikoTV, true, &m_nikoCorpse))
                LogMsg("SpiritOn: 尸体生成失败（继续，不留尸体）");
            LogMsg("SpiritOn: 步骤 尸体 结束");
        }
        else LogMsg("SpiritOn: 尸体已关闭(Corpse=0)，跳过");

        LogMsg("SpiritOn: 步骤 SetPlayerControl(false)");
        SetPlayerControl(m_player, false);
        LogMsg("SpiritOn: 步骤 FreezeCharPosition");
        FreezeCharPosition(m_niko, true);
        if (g_hide)
        {
            LogMsg("SpiritOn: 步骤 无敌+隐身");
            SetCharInvincible(m_niko, true);
            SetCharVisible(m_niko, false);
        }
        else LogMsg("SpiritOn: 隐身已关闭(HideNiko=0)，跳过");
        LogMsg("SpiritOn: 步骤 CamStart");
        CamStart();
        LogMsg("SpiritOn: CamStart 完成");
        m_hint = 0;
        m_nextScan = 0;

        m_mode = MODE_SPIRIT;
        LogMsg("灵魂出窍 niko=%d pos=%.1f,%.1f,%.1f", (int)m_niko, x, y, z);
        ShowText("TGH: spirit mode - crosshair turns green on a target, press G. H to return.", 4500);
    }

    void UpdateSpirit()
    {
        if (!Alive(m_niko)) { ReturnToNiko(); return; }

        bool fl = m_firstSpirit;
        if (fl) LogMsg("UpdateSpirit 首帧: RotateByKeys");
        RotateByKeys(true);
        if (fl) LogMsg("UpdateSpirit 首帧: LookInput");
        LookInput();
        if (fl) LogMsg("UpdateSpirit 首帧: LookInput 完成");

        f32 dx, dy, dz;
        CamDir(&dx, &dy, &dz);                       // 三维视线方向
        f32 rx = cosf(m_yaw), ry = sinf(m_yaw);      // 水平右方向
        f32 sp = SPIRIT_SPEED * (KeyDown(VK_SHIFT) ? 3.0f : 1.0f);

        if (KeyDown('W')) { m_camX += dx * sp; m_camY += dy * sp; m_camZ += dz * sp; }
        if (KeyDown('S')) { m_camX -= dx * sp; m_camY -= dy * sp; m_camZ -= dz * sp; }
        if (KeyDown('D')) { m_camX += rx * sp; m_camY += ry * sp; }
        if (KeyDown('A')) { m_camX -= rx * sp; m_camY -= ry * sp; }
        if (KeyDown(VK_SPACE) || KeyDown(VK_PRIOR)) m_camZ += sp;
        if (KeyDown('C')      || KeyDown(VK_NEXT))  m_camZ -= sp;

        if (fl) LogMsg("UpdateSpirit 首帧: SetCamPos");
        SetCamPos(m_cam, m_camX, m_camY, m_camZ);
        CamLookAhead();
        if (fl) LogMsg("UpdateSpirit 首帧: 镜头完成");

        // 每 150ms 扫描一次准星下的人，并画准星（锁定时变绿）
        u32 now = GetTickCount();
        if (g_scan && (i32)(now - m_nextScan) >= 0)
        {
            if (fl) LogMsg("UpdateSpirit 首帧: FindTarget");
            Ped t = 0;
            m_hint = FindTarget(&t) ? t : 0;
            m_nextScan = now + 150;
            if (fl) LogMsg("UpdateSpirit 首帧: FindTarget 完成");
        }
        if (g_crosshair)
        {
            if (fl) LogMsg("UpdateSpirit 首帧: DrawCrosshair");
            DrawCrosshair(m_hint != 0 && ValidTarget(m_hint));
            if (fl) LogMsg("UpdateSpirit 首帧: DrawCrosshair 完成");
        }
        if (fl) { LogMsg("UpdateSpirit 首帧全部完成"); m_firstSpirit = false; }
    }

    void DrawCrosshair(bool locked)
    {
        u8 r = locked ? 60 : 255, g = 255, b = locked ? 60 : 255;
        DrawRect(0.5f, 0.5f, 0.0030f, 0.0055f, r, g, b, 235);     // 中心点
        DrawRect(0.5f, 0.5f, 0.0140f, 0.0020f, r, g, b, 200);     // 横线
        DrawRect(0.5f, 0.5f, 0.0016f, 0.0250f, r, g, b, 200);     // 竖线
    }

    // ---- G：附身 ----
    struct Ray
    {
        f32 cx, cy, cz, dx, dy, dz;
        bool hasG;
        f32 gx, gy, gz;
    };

    bool ValidTarget(Ped c)
    {
        return c != 0 && c != m_niko && c != m_nikoCorpse && Alive(c) && !IsCharInAnyCar(c);
    }

    void ScoreCandidate(Ped c, const Ray &r, Ped *best, f32 *bestScore)
    {
        if (!ValidTarget(c)) return;

        f32 px, py, pz;
        GetCharCoordinates(c, &px, &py, &pz);

        f32 vx = px - r.cx, vy = py - r.cy, vz = pz - r.cz;
        f32 along = vx * r.dx + vy * r.dy + vz * r.dz;
        if (along <= 0.2f || along > SCAN_MAX) return;

        f32 d2 = vx * vx + vy * vy + vz * vz;
        f32 perp2 = d2 - along * along;
        if (perp2 < 0.0f) perp2 = 0.0f;
        f32 perp = sqrtf(perp2);

        // 以“视线夹角 + 距离”为主要评分。
        // 越远允许的横向误差略微增加，但不会像旧算法一样把画面边缘的人选进来。
        f32 maxPerp = 1.35f + along * 0.075f;
        if (maxPerp > 4.0f) maxPerp = 4.0f;
        if (perp > maxPerp) return;

        f32 score = perp * 3.0f + along * 0.006f;

        // 镜头落点附近的人获得明显优先级。
        if (r.hasG)
        {
            f32 ex = px - r.gx, ey = py - r.gy, ez = pz - r.gz;
            f32 gd = sqrtf(ex * ex + ey * ey + ez * ez);

            if (gd <= PICK_RADIUS)
                score -= (PICK_RADIUS - gd) * 0.55f;
        }

        if (score < *bestScore)
        {
            *bestScore = score;
            *best = c;
        }
    }

    // GetClosestChar 一次只会返回一个人。
    // 旧版只在射线中心采样，所以“前面明明有 NPC 却选不到”的情况很多。
    // 这里对每个深度同时探测中心、左右和上下几个位置，仍然只使用
    // GTA IV ScriptHook 已确认存在的 GET_CLOSEST_CHAR，不引入新 Native。
    void Probe(f32 sx, f32 sy, f32 sz, f32 rad, const Ray &r,
               Ped *best, f32 *bestScore, bool retry)
    {
        static const f32 off[9][3] =
        {
            { 0.0f,  0.0f,  0.0f },
            { 1.25f, 0.0f,  0.0f },
            {-1.25f, 0.0f,  0.0f },
            { 0.0f,  1.25f, 0.0f },
            { 0.0f, -1.25f, 0.0f },
            { 0.0f, 0.0f,  1.15f },
            { 0.0f, 0.0f, -1.15f },
            { 2.30f, 0.0f,  0.0f },
            {-2.30f, 0.0f,  0.0f }
        };

        // 把世界坐标偏移旋转到当前镜头的水平右方向。
        f32 rx = cosf(r.dx == 0.0f && r.dy == 0.0f ? 0.0f : atan2f(-r.dx, r.dy));
        f32 ry = sinf(r.dx == 0.0f && r.dy == 0.0f ? 0.0f : atan2f(-r.dx, r.dy));

        int count = retry ? 9 : 5;
        for (int i = 0; i < count; i++)
        {
            f32 ox = off[i][0], oy = off[i][1], oz = off[i][2];

            // off.x = 镜头右/左，off.y = 镜头前/后，off.z = 上/下。
            f32 wx = ox * rx + oy * r.dx;
            f32 wy = ox * ry + oy * r.dy;

            Ped c = 0;
            GetClosestChar(sx + wx, sy + wy, sz + oz, rad, true, false, &c);
            if (ValidTarget(c))
                ScoreCandidate(c, r, best, bestScore);
        }
    }

    bool FindTarget(Ped *out)
    {
        if (!out) return false;
        *out = 0;

        Ray r;
        r.cx = m_camX; r.cy = m_camY; r.cz = m_camZ;
        CamDir(&r.dx, &r.dy, &r.dz);

        r.hasG = false;
        r.gx = r.gy = r.gz = 0.0f;

        if (r.dz < -0.05f)
        {
            f32 t = (m_camZ - m_groundZ) / (-r.dz);
            if (t < 1.0f) t = 1.0f;
            if (t > SCAN_MAX) t = SCAN_MAX;

            r.gx = m_camX + r.dx * t;
            r.gy = m_camY + r.dy * t;
            r.gz = m_camZ + r.dz * t;
            r.hasG = true;
        }

        Ped best = 0;
        f32 bestScore = 1e9f;

        // 近处密一点，远处稍稀一点，减少 Native 调用同时改善近距离 NPC 命中率。
        f32 t = 0.8f;
        while (t <= SCAN_MAX)
        {
            f32 rad = 1.65f + t * 0.055f;
            if (rad > 3.8f) rad = 3.8f;

            bool retry = (t <= 25.0f);
            Probe(m_camX + r.dx * t,
                  m_camY + r.dy * t,
                  m_camZ + r.dz * t,
                  rad, r, &best, &bestScore, retry);

            t += (t < 12.0f) ? 1.0f : 1.75f;
        }

        // 如果镜头向下，额外检查准星落点周围。
        if (r.hasG)
            Probe(r.gx, r.gy, r.gz, 4.5f, r, &best, &bestScore, true);

        if (best == 0) return false;

        *out = best;
        return true;
    }

    void Possess()
    {
        if (m_mode != MODE_SPIRIT) return;

        Ped t = 0;

        // G 时永远重新扫描一次，避免 150ms 自动锁定的旧句柄/旧目标干扰。
        // 如果准星目标仍然有效且就在视线中，FindTarget 会再次确认它。
        if (!FindTarget(&t))
        {
            if (m_hint != 0 && ValidTarget(m_hint))
                t = m_hint;
            else
            {
                ShowText("TGH: no target. Aim the camera at a person.", 2500);
                LogMsg("Possess: 没找到目标");
                return;
            }
        }

        m_target = t;
        m_hint = 0;
        ClearCharTasksImmediately(m_target);
        SetBlockingOfNonTemporaryEvents(m_target, true);
        SwitchPedToRagdoll(m_target, 10000, CONVULSE_MS, 0, 1, 1, 0);

        m_convulseEnd = GetTickCount() + CONVULSE_MS;
        m_nextShake = GetTickCount();
        m_mode = MODE_CONVULSE;
        LogMsg("附身目标 ped=%d，开始抽搐 %u ms", (int)m_target, (unsigned)CONVULSE_MS);
    }

    void UpdateConvulse()
    {
        if (!Alive(m_target)) { LogMsg("抽搐中目标失效"); ReturnToNiko(); return; }

        CamFollow(m_target, CAM_JITTER);

        u32 now = GetTickCount();
        if ((i32)(now - m_nextShake) >= 0)
        {
            // 初始 Ragdoll 已经负责“倒地”。
            // 这里绝不反复重置 Ragdoll，否则 GTA IV 会不断重新接管物理，
            // 很容易出现“被弹飞 / 落地掉血”的情况。
            // 只有它意外恢复站立时才重新进入 Ragdoll。
            if (!IsPedRagdoll(m_target))
                SwitchPedToRagdoll(m_target, 10000, CONVULSE_MS, 0, 1, 1, 0);

            // 极小的横向扰动 + 极小扭转，模拟抽搐而不是“爆炸式击飞”。
            f32 fx = (Rand01() - 0.5f) * 2.0f * SHAKE_FORCE;
            f32 fy = (Rand01() - 0.5f) * 2.0f * SHAKE_FORCE;
            f32 fz = (Rand01() - 0.5f) * 2.0f * SHAKE_LIFT;
            f32 sx = (Rand01() - 0.5f) * 2.0f * SHAKE_SPIN;
            f32 sy = (Rand01() - 0.5f) * 2.0f * SHAKE_SPIN;
            f32 sz = (Rand01() - 0.5f) * 2.0f * SHAKE_SPIN;
            ApplyForceToPed(m_target, 3, fx, fy, fz, sx, sy, sz, 0, 0, 1, 1);
            m_nextShake = now + SHAKE_EVERY_MS;
        }

        if ((i32)(now - m_convulseEnd) >= 0)
        {
            FinishPossession();
        }
    }

    // 抽搐结束：把玩家变成目标 NPC
    void FinishPossession()
    {
        Ped t = m_target;
        eModel model = 0;
        GetCharModel(t, &model);

        f32 x, y, z, h = 0.0f;
        GetCharCoordinates(t, &x, &y, &z);
        GetCharHeading(t, &h);

        int dv[NUM_COMP], tv[NUM_COMP];
        SaveComponents(t, dv, tv);

        m_possModel = model;                                   // H 退出时用它生成这个 NPC 的尸体
        for (int i = 0; i < NUM_COMP; i++) { m_possDV[i] = dv[i]; m_possTV[i] = tv[i]; }

        FreezeCharPosition(m_niko, false);

        if (!SwapPlayerModel(model))
        {
            ShowText("TGH: possession failed (model not loaded).", 3000);
            SwitchPedToAnimated(t, true);
            ReturnToNiko();
            return;
        }

        ApplyComponents(m_niko, dv, tv);
        SetCharVisible(m_niko, true);
        SetCharInvincible(m_niko, false);
        DeleteChar(&m_target);               // 删掉原 NPC，避免出现两个人
        m_target = 0;

        SetCharCoordinates(m_niko, x, y, z);
        SetCharHeading(m_niko, h);
        ClearCharTasksImmediately(m_niko);

        CamStop();
        SetPlayerControl(m_player, true);
        MarkModelAsNoLongerNeeded(model);

        m_mode = MODE_POSSESSED;
        LogMsg("附身完成：玩家模型已换成 0x%08X", (unsigned)model);
        ShowText("TGH: possessed. Play normally. H = back to Niko.", 4000);
    }

    // ---- 附身后：玩家本身就是目标 NPC，正常游玩，这里只负责检测死亡 ----
    void UpdatePossessed()
    {
        Ped p = 0;
        GetPlayerChar(m_player, &p);
        if (p != 0) m_niko = p;

        if (!Alive(p))
        {
            LogMsg("附身中玩家死亡，复活后换回 Niko");
            m_mode = MODE_IDLE;
            m_needRestore = true;
        }
    }

    // ---- H：返回 ----
    void ReturnToNiko()
    {
        if (m_mode == MODE_IDLE) return;

        if (m_mode == MODE_POSSESSED)
        {
            Ped cur = 0;
            GetPlayerChar(m_player, &cur);
            if (cur != 0) m_niko = cur;

            f32 x = 0, y = 0, z = 0, h = 0;
            GetCharCoordinates(m_niko, &x, &y, &z);
            GetCharHeading(m_niko, &h);
            if (IsCharInAnyCar(m_niko))                    // 在车里就先弹出来
            {
                WarpCharFromCarToCoord(m_niko, x, y, z);
                GetCharCoordinates(m_niko, &x, &y, &z);
            }

            // 1. 被附身的 NPC 倒地死亡，变成尸体
            Ped npcCorpse = 0;
            if (!SpawnCorpse(m_possModel, x, y, z, h, m_possDV, m_possTV, false, &npcCorpse))
                LogMsg("H: NPC 尸体生成失败");

            // 2. 玩家换回 Niko（外观、武器、护甲一并恢复）
            RestoreNikoModel(true);

            // 3. Niko 回到一开始按 T 时的尸体位置，并移除那具尸体
            if (m_nikoCorpse != 0 && DoesCharExist(m_nikoCorpse))
            {
                SetCharCoordinates(m_niko, m_cX, m_cY, m_cZ);
                SetCharHeading(m_niko, m_cH);
                ClearCharTasksImmediately(m_niko);
            }
            DeleteNikoCorpse();

            m_mode = MODE_IDLE;
            LogMsg("已从附身状态换回 Niko，回到 %.1f,%.1f,%.1f", m_cX, m_cY, m_cZ);
            ShowText("TGH: back to Niko.", 2000);
            return;
        }

        if (m_target != 0 && DoesCharExist(m_target))
        {
            SwitchPedToAnimated(m_target, true);
            ClearCharTasks(m_target);
            SetBlockingOfNonTemporaryEvents(m_target, false);
        }
        CamStop();
        if (m_niko != 0 && DoesCharExist(m_niko))
        {
            FreezeCharPosition(m_niko, false);
            SetCharVisible(m_niko, true);
            SetCharInvincible(m_niko, false);
        }
        DeleteNikoCorpse();
        SetPlayerControl(m_player, true);

        m_target = 0;
        m_hint = 0;
        m_mode = MODE_IDLE;
        LogMsg("已返回 Niko");
        ShowText("TGH: back to Niko.", 2000);
    }

protected:
    virtual void RunScript()
    {
        LogMsg("TGHMod v6 脚本线程启动");
        LoadIni();
        ShowText("TGHMod loaded. T = spirit, G = possess, H = return.", 5000);

        for (;;)
        {
            bool pT = Pressed('T');
            bool pG = Pressed('G');
            bool pH = Pressed('H');

            if (m_needRestore)
            {
                Ped p = 0;
                m_player = ConvertIntToPlayerIndex(GetPlayerId());
                GetPlayerChar(m_player, &p);
                if (Alive(p)) { m_niko = p; RestoreNikoModel(false); DeleteNikoCorpse(); m_needRestore = false; }
            }

            if (pT) SpiritOn();
            if (pG) Possess();
            if (pH) ReturnToNiko();

            switch (m_mode)
            {
            case MODE_SPIRIT:    UpdateSpirit();    break;
            case MODE_CONVULSE:  UpdateConvulse();  break;
            case MODE_POSSESSED: UpdatePossessed(); break;
            default: break;
            }

            Wait(0);
        }
    }

public:
    TGHThread()
        : NativeFiberThread(),
          m_mode(MODE_IDLE), m_player(0), m_niko(0), m_target(0), m_cam(0), m_camOn(false),
          m_camX(0), m_camY(0), m_camZ(0), m_yaw(0), m_pitch(0), m_groundZ(0),
          m_convulseEnd(0), m_nextShake(0), m_rng(12345u),
          m_nikoModel(0), m_needRestore(false),
          m_mouseInit(false), m_lastX(0), m_lastY(0), m_firstLook(true), m_wCount(0), m_wCur(0), m_nikoArmour(0),
          m_nikoCorpse(0), m_cX(0), m_cY(0), m_cZ(0), m_cH(0), m_possModel(0), m_hint(0), m_firstSpirit(false), m_nextScan(0)
    {
        for (int i = 0; i < NUM_COMP; i++) { m_possDV[i] = 0; m_possTV[i] = 0; }
        for (int i = 0; i < NUM_WEAPON_SLOTS; i++) { m_wList[i] = 0; m_wAmmo[i] = 0; }
        for (int i = 0; i < NUM_COMP; i++) { m_nikoDV[i] = 0; m_nikoTV[i] = 0; }
        SetName("TGHMod");
    }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        LogMsg("TGHMod.asi 已加载（DllMain）");
        ScriptHookManager::RegisterThread(new TGHThread(), hModule);
    }
    return TRUE;
}
