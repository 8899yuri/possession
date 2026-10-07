// TGHMod  (C++ ASI, 基于 Aru 的 GTA IV ScriptHook SDK)   —— v2
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
// 附身状态：完全就是正常玩游戏（走、跑、攻击、上车都可以），按 H 变回 Niko
//
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
static const f32 SHAKE_FORCE    = 0.35f;  // 抽搐时每次施加的小力（太猛/没反应就改这个）
static const u32 SHAKE_EVERY_MS = 120;    // 抽搐施力间隔
static const f32 SPIRIT_SPEED   = 0.35f;  // 灵魂镜头每帧移动距离
static const f32 PICK_RADIUS    = 5.0f;   // 选目标：镜头视线落点附近多少米内找人
static const f32 FOLLOW_DIST    = 5.0f;   // 附身后镜头距离
static const f32 FOLLOW_HEIGHT  = 2.2f;   // 附身后镜头高度
static const int NUM_COMP       = 11;     // 复制外观时处理的部件数（head..face）

static const f32 PI_F = 3.14159265f;

// ---------------- 日志 ----------------
static void LogMsg(const char *fmt, ...)
{
    FILE *f = fopen("TGHMod.log", "a");
    if (!f) return;
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
        for (int c = 0; c < NUM_COMP; c++)
        {
            dv[c] = (int)GetCharDrawableVariation(p, c);
            tv[c] = (int)GetCharTextureVariation(p, c);
        }
    }

    void ApplyComponents(Ped p, const int *dv, const int *tv)
    {
        for (int c = 0; c < NUM_COMP; c++)
            SetCharComponentVariation(p, c, (u32)dv[c], (u32)tv[c]);
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

    // 把玩家换回 Niko 的模型和外观
    void RestoreNikoModel()
    {
        m_player = ConvertIntToPlayerIndex(GetPlayerId());
        if (SwapPlayerModel(m_nikoModel))
            ApplyComponents(m_niko, m_nikoDV, m_nikoTV);
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
    void CamFollow(Ped p)
    {
        f32 x, y, z;
        GetCharCoordinates(p, &x, &y, &z);
        f32 fx = -sinf(m_yaw), fy = cosf(m_yaw);
        m_camX = x - fx * FOLLOW_DIST;
        m_camY = y - fy * FOLLOW_DIST;
        m_camZ = z + FOLLOW_HEIGHT;
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

        f32 x, y, z, h = 0.0f;
        GetCharCoordinates(m_niko, &x, &y, &z);
        GetCharHeading(m_niko, &h);

        GetCharModel(m_niko, &m_nikoModel);
        SaveComponents(m_niko, m_nikoDV, m_nikoTV);

        m_yaw = h * PI_F / 180.0f;
        m_pitch = -0.35f;
        m_camX = x; m_camY = y; m_camZ = z + 3.0f;
        m_groundZ = z - 1.0f;

        SetPlayerControl(m_player, false);
        FreezeCharPosition(m_niko, true);
        CamStart();

        m_mode = MODE_SPIRIT;
        LogMsg("灵魂出窍 niko=%d pos=%.1f,%.1f,%.1f", (int)m_niko, x, y, z);
        ShowText("TGH: spirit mode - aim at a person, press G. H to return.", 4000);
    }

    void UpdateSpirit()
    {
        if (!Alive(m_niko)) { ReturnToNiko(); return; }

        RotateByKeys(true);

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

        SetCamPos(m_cam, m_camX, m_camY, m_camZ);
        CamLookAhead();
    }

    // ---- G：附身 ----
    bool FindTarget(Ped *out)
    {
        f32 dx, dy, dz;
        CamDir(&dx, &dy, &dz);

        // 视线落点：往下看时取视线与地面的交点，否则取前方 12 米
        f32 t = 12.0f;
        if (dz < -0.05f)
        {
            t = (m_camZ - m_groundZ) / (-dz);
            if (t < 3.0f) t = 3.0f;
            if (t > 40.0f) t = 40.0f;
        }
        f32 px = m_camX + dx * t;
        f32 py = m_camY + dy * t;
        f32 pz = m_camZ + dz * t;

        Ped cand = 0;
        GetClosestChar(px, py, pz, PICK_RADIUS, true, false, &cand);

        if (cand == 0 || cand == m_niko) return false;
        if (!Alive(cand)) return false;
        if (IsCharInAnyCar(cand)) return false;   // 暂不支持附身车里的人
        *out = cand;
        return true;
    }

    void Possess()
    {
        if (m_mode != MODE_SPIRIT) return;

        Ped t = 0;
        if (!FindTarget(&t))
        {
            ShowText("TGH: no target. Aim the camera at a person.", 2500);
            LogMsg("Possess: 没找到目标");
            return;
        }

        m_target = t;
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

        CamFollow(m_target);

        u32 now = GetTickCount();
        if ((i32)(now - m_nextShake) >= 0)
        {
            f32 fx = (Rand01() - 0.5f) * 2.0f * SHAKE_FORCE;
            f32 fy = (Rand01() - 0.5f) * 2.0f * SHAKE_FORCE;
            ApplyForceToPed(m_target, 3, fx, fy, 0.05f, 0.0f, 0.0f, 0.0f, 0, 0, 1, 1);
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

        FreezeCharPosition(m_niko, false);

        if (!SwapPlayerModel(model))
        {
            ShowText("TGH: possession failed (model not loaded).", 3000);
            SwitchPedToAnimated(t, true);
            ReturnToNiko();
            return;
        }

        ApplyComponents(m_niko, dv, tv);
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
            RestoreNikoModel();
            m_mode = MODE_IDLE;
            LogMsg("已从附身状态换回 Niko");
            ShowText("TGH: back to Niko.", 2000);
            return;
        }

        if (m_target != 0 && DoesCharExist(m_target))
        {
            ClearCharTasks(m_target);
            SetBlockingOfNonTemporaryEvents(m_target, false);
        }
        CamStop();
        if (m_niko != 0 && DoesCharExist(m_niko))
        {
            FreezeCharPosition(m_niko, false);
        }
        SetPlayerControl(m_player, true);

        m_target = 0;
        m_mode = MODE_IDLE;
        LogMsg("已返回 Niko");
        ShowText("TGH: back to Niko.", 2000);
    }

protected:
    virtual void RunScript()
    {
        LogMsg("TGHMod 脚本线程启动");
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
                if (Alive(p)) { m_niko = p; RestoreNikoModel(); m_needRestore = false; }
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
          m_nikoModel(0), m_needRestore(false)
    {
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
