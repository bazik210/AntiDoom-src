#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define INI_FILE L".\\launcher.ini"

// Control IDs
#define IDC_IWAD_PATH       101
#define IDC_IWAD_BROWSE     102
#define IDC_IWAD_PRESET     103
#define IDC_PWAD_PATH       104
#define IDC_PWAD_BROWSE     105
#define IDC_RES_COMBO       106
#define IDC_CHK_FULLSCREEN  107
#define IDC_CHK_MAXIMIZED   108
#define IDC_CHK_KEEPASPECT  109
#define IDC_CHK_BOT         110
#define IDC_CHK_MLOOK       111
#define IDC_CHK_MENUMOUSE   112
#define IDC_CHK_FAST        113
#define IDC_CHK_NOMONSTERS  114
#define IDC_CHK_RESPAWN     115
#define IDC_SKILL_COMBO     116
#define IDC_WARP_EDIT       117
#define IDC_EXTRA_EDIT      118
#define IDC_CHK_CLOSE_START 119
#define IDC_BTN_LAUNCH      120
#define IDC_BTN_SAVE        121
#define IDC_BTN_EXIT        122
#define IDC_CHK_QUICKSTART  123
#define IDC_CHK_JUMP        124
#define IDC_BTN_LANG        125
#define IDC_CHK_SMOOTH      126

// Static Label IDs for dynamic translation
#define IDC_LBL_IWAD        201
#define IDC_LBL_PRESET      202
#define IDC_LBL_PWAD        203
#define IDC_LBL_RES         204
#define IDC_LBL_SKILL       205
#define IDC_LBL_WARP        206
#define IDC_LBL_EXTRA       207

static HWND hIwadPath, hIwadPreset, hPwadPath;
static HWND hResCombo, hChkFullscreen, hChkMaximized, hChkKeepAspect, hChkSmooth;
static HWND hChkBot, hChkMlook, hChkMenuMouse, hChkFast, hChkNoMonsters, hChkRespawn, hChkJUMP;
static HWND hSkillCombo, hWarpEdit, hChkQuickStart, hExtraEdit, hChkCloseStart;
static HWND hBtnLaunch, hBtnSave, hBtnExit, hIwadBrowse, hPwadBrowse, hBtnLang;

// Static Label HWNDs
static HWND hLblIwad, hLblPreset, hLblPwad, hLblRes, hLblSkill, hLblWarp, hLblExtra;
static HWND hMainWnd = NULL;

static HFONT hFontTitle, hFontSub, hFontMain, hFontBold, hFontLaunch, hFontBtn;
static HBRUSH hBrushBg, hBrushPanel, hBrushEdit;
static COLORREF colBg = RGB(24, 26, 31);
static COLORREF colPanel = RGB(36, 39, 47);
static COLORREF colText = RGB(225, 230, 240);
static COLORREF colGold = RGB(235, 195, 75);
static COLORREF colGray = RGB(150, 155, 168);

// Language: 0 = Russian, 1 = English
static int current_lang = 0;

typedef struct {
    int w;
    int h;
    wchar_t label[64];
} ResOption;

static ResOption res_options[16];
static int res_count = 0;

typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t label[128];
} FoundWad;

static FoundWad found_wads[64];
static int found_wad_count = 0;

static const wchar_t* skill_list_ru[] = {
    L"1: I'm too young to die (Легко)",
    L"2: Hey, not too rough (Умеренно)",
    L"3: Hurt me plenty (Нормально)",
    L"4: Ultra-Violence (Сложно)",
    L"5: Nightmare! (Кошмар)"
};

static const wchar_t* skill_list_en[] = {
    L"1: I'm too young to die (Easy)",
    L"2: Hey, not too rough (Medium)",
    L"3: Hurt me plenty (Normal)",
    L"4: Ultra-Violence (Hard)",
    L"5: Nightmare! (Nightmare)"
};

static void GetLauncherDir(wchar_t* outDir, int maxLen)
{
    GetModuleFileNameW(NULL, outDir, maxLen);
    wchar_t *p = wcsrchr(outDir, L'\\');
    if (p) *p = L'\0';
}

static void InitResolutions(void)
{
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    if (scrW <= 0) scrW = 1920;
    if (scrH <= 0) scrH = 1080;

    res_count = 0;

    // #0 is ALWAYS native screen resolution (Default!)
    res_options[res_count].w = scrW;
    res_options[res_count].h = scrH;
    if (current_lang == 0)
        swprintf(res_options[res_count].label, 64, L"%dx%d (Текущий экран)", scrW, scrH);
    else
        swprintf(res_options[res_count].label, 64, L"%dx%d (Native Screen)", scrW, scrH);
    res_count++;

    static const struct { int w; int h; const wchar_t* desc_ru; const wchar_t* desc_en; } presets[] = {
        { 1920, 1080, L"Full HD", L"Full HD" },
        { 1920, 1200, L"16:10 Full HD", L"16:10 Full HD" },
        { 2560, 1440, L"2K QHD", L"2K QHD" },
        { 3840, 2160, L"4K UHD", L"4K UHD" },
        { 1600, 1000, L"5x (16:10)", L"5x (16:10)" },
        { 1280, 800,  L"4x HD (16:10)", L"4x HD (16:10)" },
        { 960,  600,  L"3x (16:10)", L"3x (16:10)" },
        { 640,  400,  L"2x Классика", L"2x Classic" }
    };

    for (int i = 0; i < (int)(sizeof(presets)/sizeof(presets[0])); i++) {
        if (presets[i].w == scrW && presets[i].h == scrH)
            continue; // avoid duplicate
        res_options[res_count].w = presets[i].w;
        res_options[res_count].h = presets[i].h;
        const wchar_t* desc = (current_lang == 0) ? presets[i].desc_ru : presets[i].desc_en;
        swprintf(res_options[res_count].label, 64, L"%dx%d (%ls)", presets[i].w, presets[i].h, desc);
        res_count++;
    }
}

static void GetWadTitle(const wchar_t *path, const wchar_t *fileName, wchar_t *outTitle, size_t maxChars)
{
    FILE *f = _wfopen(path, L"rb");
    if (f) {
        char magic[4];
        int numlumps = 0, infotableofs = 0;
        if (fread(magic, 4, 1, f) == 1 && (memcmp(magic, "IWAD", 4) == 0 || memcmp(magic, "PWAD", 4) == 0) &&
            fread(&numlumps, 4, 1, f) == 1 &&
            fread(&infotableofs, 4, 1, f) == 1) {
            if (numlumps > 0 && numlumps < 50000 && fseek(f, infotableofs, SEEK_SET) == 0) {
                int has_map01 = 0, has_e1m1 = 0, has_e2m1 = 0, has_e4m1 = 0, has_m_epi4 = 0;
                int has_camo1 = 0, has_redmin6 = 0;
                for (int i = 0; i < numlumps; i++) {
                    int filepos, size;
                    char name[9] = {0};
                    if (fread(&filepos, 4, 1, f) != 1 ||
                        fread(&size, 4, 1, f) != 1 ||
                        fread(name, 8, 1, f) != 1) break;
                    for (int c = 0; c < 8 && name[c]; c++) {
                        if (name[c] >= 'a' && name[c] <= 'z') name[c] -= 32;
                    }
                    if (strcmp(name, "MAP01") == 0) has_map01 = 1;
                    else if (strcmp(name, "E1M1") == 0) has_e1m1 = 1;
                    else if (strcmp(name, "E2M1") == 0) has_e2m1 = 1;
                    else if (strcmp(name, "E4M1") == 0) has_e4m1 = 1;
                    else if (strcmp(name, "M_EPI4") == 0) has_m_epi4 = 1;
                    else if (strcmp(name, "CAMO1") == 0) has_camo1 = 1;
                    else if (strcmp(name, "REDMIN6") == 0) has_redmin6 = 1;
                }
                fclose(f);
                if (has_map01) {
                    if (has_camo1 || wcsstr(fileName, L"PLUTONIA") || wcsstr(fileName, L"plutonia"))
                        wcsncpy(outTitle, L"Final DOOM: Plutonia", maxChars);
                    else if (has_redmin6 || wcsstr(fileName, L"TNT") || wcsstr(fileName, L"tnt"))
                        wcsncpy(outTitle, L"Final DOOM: TNT Evilution", maxChars);
                    else
                        wcsncpy(outTitle, L"DOOM II: Hell on Earth", maxChars);
                    return;
                } else if (has_e1m1) {
                    if (has_e4m1 && has_m_epi4)
                        wcsncpy(outTitle, L"The Ultimate DOOM", maxChars);
                    else if (has_e2m1)
                        wcsncpy(outTitle, (current_lang == 0) ? L"DOOM (3 эпизода)" : L"DOOM (3 Episodes)", maxChars);
                    else
                        wcsncpy(outTitle, (current_lang == 0) ? L"DOOM (Shareware, 1 эпизод)" : L"DOOM (Shareware, 1 Episode)", maxChars);
                    return;
                }
            }
        }
        if (f) fclose(f);
    }
    outTitle[0] = L'\0';
}

static void ScanWadFolder(const wchar_t* targetPath)
{
    wchar_t dir[MAX_PATH] = {0};
    if (targetPath && targetPath[0]) {
        wcscpy(dir, targetPath);
        wchar_t *pSlash = wcsrchr(dir, L'\\');
        if (!pSlash) pSlash = wcsrchr(dir, L'/');
        if (pSlash) {
            *pSlash = L'\0';
        } else {
            wcscpy(dir, L"wad");
        }
    } else {
        wcscpy(dir, L"wad");
    }

    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(dir, L".");
    }

    wchar_t searchPattern[MAX_PATH];
    swprintf(searchPattern, MAX_PATH, L"%ls\\*.wad", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    found_wad_count = 0;

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (found_wad_count >= 64) break;

            if (wcscmp(dir, L".") == 0) {
                swprintf(found_wads[found_wad_count].path, MAX_PATH, L"%ls", fd.cFileName);
            } else {
                swprintf(found_wads[found_wad_count].path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            }

            wchar_t title[64] = {0};
            GetWadTitle(found_wads[found_wad_count].path, fd.cFileName, title, 64);
            if (title[0]) {
                swprintf(found_wads[found_wad_count].label, 128, L"%ls (%ls)", found_wads[found_wad_count].path, title);
            } else {
                swprintf(found_wads[found_wad_count].label, 128, L"%ls", found_wads[found_wad_count].path);
            }

            found_wad_count++;
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    SendMessageW(hIwadPreset, CB_RESETCONTENT, 0, 0);
    int matched = -1;
    for (int i = 0; i < found_wad_count; i++) {
        SendMessageW(hIwadPreset, CB_ADDSTRING, 0, (LPARAM)found_wads[i].label);
        if (targetPath && _wcsicmp(targetPath, found_wads[i].path) == 0) {
            matched = i;
        }
    }

    if (found_wad_count > 0) {
        if (matched >= 0) {
            SendMessageW(hIwadPreset, CB_SETCURSEL, matched, 0);
        } else {
            SendMessageW(hIwadPreset, CB_SETCURSEL, 0, 0);
        }
    } else {
        SendMessageW(hIwadPreset, CB_ADDSTRING, 0, (LPARAM)(current_lang == 0 ? L"(WAD файлы в папке не найдены)" : L"(No WAD files found in folder)"));
        SendMessageW(hIwadPreset, CB_SETCURSEL, 0, 0);
    }
}

static void ApplyLanguage(int lang)
{
    current_lang = lang;

    if (current_lang == 0) {
        // Russian
        SetWindowTextW(hMainWnd, L"AntiDoom - Win32 Launcher");
        SetWindowTextW(hBtnLang, L"[ EN ]");

        SetWindowTextW(hLblIwad, L"Файл игры (IWAD):");
        SetWindowTextW(hIwadBrowse, L"Обзор...");
        SetWindowTextW(hLblPreset, L"Обнаруженные WAD:");
        SetWindowTextW(hLblPwad, L"Дополнительный мод / PWAD (-file):");
        SetWindowTextW(hPwadBrowse, L"Обзор...");

        SetWindowTextW(hLblRes, L"Разрешение экрана:");
        SetWindowTextW(hChkKeepAspect, L"Сохранять пропорции (-keepaspect)");
        SetWindowTextW(hChkSmooth, L"Сглаживание (Bilinear) (-smooth)");
        SetWindowTextW(hChkFullscreen, L"Полный экран (-fullscreen)");
        SetWindowTextW(hChkMaximized, L"Окно без рамок (-maximized)");

        SetWindowTextW(hChkBot, L"Включить бота (-bot)");
        SetWindowTextW(hChkMlook, L"Обзор мышью (-mlook)");
        SetWindowTextW(hChkMenuMouse, L"Курсор мыши в меню (-menumouse)");
        SetWindowTextW(hChkFast, L"Быстрые монстры (-fast)");
        SetWindowTextW(hChkNoMonsters, L"Без монстров (-nomonsters)");
        SetWindowTextW(hChkRespawn, L"Возрождение монстров (-respawn)");
        SetWindowTextW(hChkJUMP, L"Прыжок (Space) (-jump)");
        SetWindowTextW(hChkQuickStart, L"Сразу в игру (минуя заставку)");

        SetWindowTextW(hLblSkill, L"Сложность:");
        SetWindowTextW(hLblWarp, L"Карта (Warp):");
        SetWindowTextW(hLblExtra, L"Дополнительные параметры:");
        SetWindowTextW(hChkCloseStart, L"Закрывать ланчер при запуске игры");

        SetWindowTextW(hBtnLaunch, L"►  ЗАПУСТИТЬ ANTIDOOM");
        SetWindowTextW(hBtnSave, L"Сохранить настройки");
        SetWindowTextW(hBtnExit, L"Выход");
    } else {
        // English
        SetWindowTextW(hMainWnd, L"AntiDoom - Win32 Launcher");
        SetWindowTextW(hBtnLang, L"[ RU ]");

        SetWindowTextW(hLblIwad, L"Game IWAD File:");
        SetWindowTextW(hIwadBrowse, L"Browse...");
        SetWindowTextW(hLblPreset, L"Discovered WADs:");
        SetWindowTextW(hLblPwad, L"Additional Mod / PWAD (-file):");
        SetWindowTextW(hPwadBrowse, L"Browse...");

        SetWindowTextW(hLblRes, L"Screen Resolution:");
        SetWindowTextW(hChkKeepAspect, L"Preserve Aspect Ratio (-keepaspect)");
        SetWindowTextW(hChkSmooth, L"Smooth Scaling (Bilinear) (-smooth)");
        SetWindowTextW(hChkFullscreen, L"Fullscreen Mode (-fullscreen)");
        SetWindowTextW(hChkMaximized, L"Borderless Window (-maximized)");

        SetWindowTextW(hChkBot, L"Enable AI Bot (-bot)");
        SetWindowTextW(hChkMlook, L"Mouse Freelook (-mlook)");
        SetWindowTextW(hChkMenuMouse, L"Mouse in Menus (-menumouse)");
        SetWindowTextW(hChkFast, L"Fast Monsters (-fast)");
        SetWindowTextW(hChkNoMonsters, L"No Monsters (-nomonsters)");
        SetWindowTextW(hChkRespawn, L"Respawn Monsters (-respawn)");
        SetWindowTextW(hChkJUMP, L"Enable Jump (Space) (-jump)");
        SetWindowTextW(hChkQuickStart, L"Quick Start (skip title screen)");

        SetWindowTextW(hLblSkill, L"Difficulty:");
        SetWindowTextW(hLblWarp, L"Map (Warp):");
        SetWindowTextW(hLblExtra, L"Extra Parameters:");
        SetWindowTextW(hChkCloseStart, L"Close launcher when game starts");

        SetWindowTextW(hBtnLaunch, L"►  LAUNCH ANTIDOOM");
        SetWindowTextW(hBtnSave, L"Save Settings");
        SetWindowTextW(hBtnExit, L"Exit");
    }

    // Refresh Skill Combobox preserving selection
    int curSkill = (int)SendMessageW(hSkillCombo, CB_GETCURSEL, 0, 0);
    SendMessageW(hSkillCombo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 5; i++) {
        SendMessageW(hSkillCombo, CB_ADDSTRING, 0, (LPARAM)(current_lang == 0 ? skill_list_ru[i] : skill_list_en[i]));
    }
    SendMessageW(hSkillCombo, CB_SETCURSEL, (curSkill >= 0 && curSkill < 5) ? curSkill : 2, 0);

    // Refresh Resolution Combobox preserving selection
    int curRes = (int)SendMessageW(hResCombo, CB_GETCURSEL, 0, 0);
    InitResolutions();
    SendMessageW(hResCombo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < res_count; i++) {
        SendMessageW(hResCombo, CB_ADDSTRING, 0, (LPARAM)res_options[i].label);
    }
    SendMessageW(hResCombo, CB_SETCURSEL, (curRes >= 0 && curRes < res_count) ? curRes : 0, 0);

    // Refresh WAD Preset Combobox
    wchar_t curIwad[512] = {0};
    GetWindowTextW(hIwadPath, curIwad, 512);
    ScanWadFolder(curIwad);

    if (hMainWnd) {
        InvalidateRect(hMainWnd, NULL, TRUE);
    }
}

static void SaveSettings(void)
{
    wchar_t buf[512];

    WritePrivateProfileStringW(L"Launcher", L"Language", current_lang == 1 ? L"en" : L"ru", INI_FILE);

    GetWindowTextW(hIwadPath, buf, sizeof(buf)/sizeof(wchar_t));
    WritePrivateProfileStringW(L"Launcher", L"IWAD", buf, INI_FILE);

    GetWindowTextW(hPwadPath, buf, sizeof(buf)/sizeof(wchar_t));
    WritePrivateProfileStringW(L"Launcher", L"PWAD", buf, INI_FILE);

    int sel = (int)SendMessageW(hResCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= res_count) sel = 0;
    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", res_options[sel].w);
    WritePrivateProfileStringW(L"Launcher", L"Width", buf, INI_FILE);
    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", res_options[sel].h);
    WritePrivateProfileStringW(L"Launcher", L"Height", buf, INI_FILE);
    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", sel);
    WritePrivateProfileStringW(L"Launcher", L"Resolution", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkFullscreen, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"Fullscreen", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkMaximized, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"Maximized", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkKeepAspect, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"KeepAspect", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkSmooth, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"Smooth", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkBot, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"Bot", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkMlook, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"Mlook", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkMenuMouse, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"MenuMouse", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkFast, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"Fast", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkNoMonsters, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"NoMonsters", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkRespawn, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"Respawn", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkJUMP, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"Jump", buf, INI_FILE);

    sel = (int)SendMessageW(hSkillCombo, CB_GETCURSEL, 0, 0);
    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", sel >= 0 ? sel : 2);
    WritePrivateProfileStringW(L"Launcher", L"Skill", buf, INI_FILE);

    GetWindowTextW(hWarpEdit, buf, sizeof(buf)/sizeof(wchar_t));
    WritePrivateProfileStringW(L"Launcher", L"Warp", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkQuickStart, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"QuickStart", buf, INI_FILE);

    GetWindowTextW(hExtraEdit, buf, sizeof(buf)/sizeof(wchar_t));
    WritePrivateProfileStringW(L"Launcher", L"ExtraArgs", buf, INI_FILE);

    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L"%d", (int)SendMessageW(hChkCloseStart, BM_GETCHECK, 0, 0));
    WritePrivateProfileStringW(L"Launcher", L"CloseOnStart", buf, INI_FILE);
}

static void LoadSettings(void)
{
    wchar_t buf[512];

    GetPrivateProfileStringW(L"Launcher", L"Language", L"ru", buf, sizeof(buf)/sizeof(wchar_t), INI_FILE);
    if (_wcsicmp(buf, L"en") == 0) {
        current_lang = 1;
    } else {
        current_lang = 0;
    }

    GetPrivateProfileStringW(L"Launcher", L"IWAD", L"", buf, sizeof(buf)/sizeof(wchar_t), INI_FILE);
    if (buf[0] == L'\0') {
        if (GetFileAttributesW(L"wad\\DOOM2.WAD") != INVALID_FILE_ATTRIBUTES)
            wcscpy(buf, L"wad\\DOOM2.WAD");
        else
            wcscpy(buf, L"wad\\DOOM2.WAD");
    }
    SetWindowTextW(hIwadPath, buf);

    GetPrivateProfileStringW(L"Launcher", L"PWAD", L"", buf, sizeof(buf)/sizeof(wchar_t), INI_FILE);
    SetWindowTextW(hPwadPath, buf);

    int savedW = GetPrivateProfileIntW(L"Launcher", L"Width", 0, INI_FILE);
    int savedH = GetPrivateProfileIntW(L"Launcher", L"Height", 0, INI_FILE);
    int selRes = 0; // Default: native screen resolution (index 0)
    if (savedW > 0 && savedH > 0) {
        for (int i = 0; i < res_count; i++) {
            if (res_options[i].w == savedW && res_options[i].h == savedH) {
                selRes = i;
                break;
            }
        }
    }
    SendMessageW(hResCombo, CB_SETCURSEL, selRes, 0);

    SendMessageW(hChkFullscreen, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"Fullscreen", 0, INI_FILE), 0);
    SendMessageW(hChkMaximized, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"Maximized", 0, INI_FILE), 0);
    SendMessageW(hChkKeepAspect, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"KeepAspect", 1, INI_FILE), 0);
    SendMessageW(hChkSmooth, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"Smooth", 0, INI_FILE), 0);

    SendMessageW(hChkBot, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"Bot", 0, INI_FILE), 0);
    SendMessageW(hChkMlook, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"Mlook", 1, INI_FILE), 0);
    SendMessageW(hChkMenuMouse, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"MenuMouse", 1, INI_FILE), 0);
    SendMessageW(hChkFast, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"Fast", 0, INI_FILE), 0);
    SendMessageW(hChkNoMonsters, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"NoMonsters", 0, INI_FILE), 0);
    SendMessageW(hChkRespawn, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"Respawn", 0, INI_FILE), 0);
    SendMessageW(hChkJUMP, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"Jump", 1, INI_FILE), 0);

    int skill = GetPrivateProfileIntW(L"Launcher", L"Skill", 2, INI_FILE);
    SendMessageW(hSkillCombo, CB_SETCURSEL, skill, 0);

    GetPrivateProfileStringW(L"Launcher", L"Warp", L"", buf, sizeof(buf)/sizeof(wchar_t), INI_FILE);
    SetWindowTextW(hWarpEdit, buf);

    SendMessageW(hChkQuickStart, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"QuickStart", 0, INI_FILE), 0);

    GetPrivateProfileStringW(L"Launcher", L"ExtraArgs", L"", buf, sizeof(buf)/sizeof(wchar_t), INI_FILE);
    SetWindowTextW(hExtraEdit, buf);

    SendMessageW(hChkCloseStart, BM_SETCHECK, GetPrivateProfileIntW(L"Launcher", L"CloseOnStart", 1, INI_FILE), 0);

    ApplyLanguage(current_lang);
}

static void BrowseFile(HWND hParent, HWND hTargetEdit, const wchar_t* title)
{
    wchar_t launcherDir[MAX_PATH] = {0};
    GetLauncherDir(launcherDir, MAX_PATH);

    IFileOpenDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&pfd);
    if (SUCCEEDED(hr) && pfd) {
        pfd->lpVtbl->SetTitle(pfd, title);

        IShellItem *psiFolder = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(launcherDir, NULL, &IID_IShellItem, (void**)&psiFolder))) {
            pfd->lpVtbl->SetFolder(pfd, psiFolder);
            pfd->lpVtbl->SetDefaultFolder(pfd, psiFolder);
            psiFolder->lpVtbl->Release(psiFolder);
        }

        COMDLG_FILTERSPEC rgSpec[] = {
            { L"DOOM WAD (*.wad)", L"*.wad" },
            { (current_lang == 0 ? L"Все файлы (*.*)" : L"All Files (*.*)"), L"*.*" }
        };
        pfd->lpVtbl->SetFileTypes(pfd, 2, rgSpec);

        FILEOPENDIALOGOPTIONS opt;
        if (SUCCEEDED(pfd->lpVtbl->GetOptions(pfd, &opt))) {
            pfd->lpVtbl->SetOptions(pfd, opt | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        }

        if (SUCCEEDED(pfd->lpVtbl->Show(pfd, hParent))) {
            IShellItem *psiResult = NULL;
            if (SUCCEEDED(pfd->lpVtbl->GetResult(pfd, &psiResult))) {
                PWSTR pszFilePath = NULL;
                if (SUCCEEDED(psiResult->lpVtbl->GetDisplayName(psiResult, SIGDN_FILESYSPATH, &pszFilePath))) {
                    int curLen = wcslen(launcherDir);
                    if (_wcsnicmp(pszFilePath, launcherDir, curLen) == 0 && pszFilePath[curLen] == L'\\') {
                        SetWindowTextW(hTargetEdit, pszFilePath + curLen + 1);
                        if (hTargetEdit == hIwadPath) ScanWadFolder(pszFilePath + curLen + 1);
                    } else {
                        SetWindowTextW(hTargetEdit, pszFilePath);
                        if (hTargetEdit == hIwadPath) ScanWadFolder(pszFilePath);
                    }
                    CoTaskMemFree(pszFilePath);
                }
                psiResult->lpVtbl->Release(psiResult);
            }
        }
        pfd->lpVtbl->Release(pfd);
        return;
    }

    // Classic Fallback
    wchar_t fileBuf[MAX_PATH] = {0};
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hParent;
    ofn.lpstrFilter = L"DOOM WAD (*.wad)\0*.wad\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf)/sizeof(wchar_t);
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = launcherDir;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        int curLen = wcslen(launcherDir);
        if (_wcsnicmp(fileBuf, launcherDir, curLen) == 0 && fileBuf[curLen] == L'\\') {
            SetWindowTextW(hTargetEdit, fileBuf + curLen + 1);
            if (hTargetEdit == hIwadPath) ScanWadFolder(fileBuf + curLen + 1);
        } else {
            SetWindowTextW(hTargetEdit, fileBuf);
            if (hTargetEdit == hIwadPath) ScanWadFolder(fileBuf);
        }
    }
}

static void LaunchGame(HWND hWnd)
{
    SaveSettings();

    // Prefer antidoom.exe, fallback to doom2.exe
    const wchar_t *exeName = L"antidoom.exe";
    if (GetFileAttributesW(L"antidoom.exe") == INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(L"doom2.exe") != INVALID_FILE_ATTRIBUTES) {
            exeName = L"doom2.exe";
        }
    }

    wchar_t cmd[2048];
    swprintf(cmd, 2048, L"%ls", exeName);
    wchar_t buf[512];

    // IWAD
    GetWindowTextW(hIwadPath, buf, sizeof(buf)/sizeof(wchar_t));
    if (buf[0]) {
        wcscat(cmd, L" -iwad \"");
        wcscat(cmd, buf);
        wcscat(cmd, L"\"");
    }

    // PWAD
    GetWindowTextW(hPwadPath, buf, sizeof(buf)/sizeof(wchar_t));
    if (buf[0]) {
        wcscat(cmd, L" -file \"");
        wcscat(cmd, buf);
        wcscat(cmd, L"\"");
    }

    // Resolution
    int resIdx = (int)SendMessageW(hResCombo, CB_GETCURSEL, 0, 0);
    int rw = res_options[0].w, rh = res_options[0].h;
    if (resIdx >= 0 && resIdx < res_count) {
        rw = res_options[resIdx].w;
        rh = res_options[resIdx].h;
    }
    swprintf(buf, sizeof(buf)/sizeof(wchar_t), L" -width %d -height %d", rw, rh);
    wcscat(cmd, buf);

    if (SendMessageW(hChkFullscreen, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -fullscreen");
    if (SendMessageW(hChkMaximized, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -maximized");
    if (SendMessageW(hChkKeepAspect, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -keepaspect");
    if (SendMessageW(hChkSmooth, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -smooth");

    if (SendMessageW(hChkBot, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -bot");
    if (!SendMessageW(hChkMlook, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -nomlook");
    if (!SendMessageW(hChkMenuMouse, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -nomenumouse");

    if (SendMessageW(hChkFast, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -fast");
    if (SendMessageW(hChkNoMonsters, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -nomonsters");
    if (SendMessageW(hChkRespawn, BM_GETCHECK, 0, 0)) wcscat(cmd, L" -respawn");

    // Jumping
    if (SendMessageW(hChkJUMP, BM_GETCHECK, 0, 0)) {
        wcscat(cmd, L" -jump");
    } else {
        wcscat(cmd, L" -nojump");
    }

    int skill = (int)SendMessageW(hSkillCombo, CB_GETCURSEL, 0, 0);
    if (skill >= 0 && skill < 5) {
        swprintf(buf, sizeof(buf)/sizeof(wchar_t), L" -skill %d", skill + 1);
        wcscat(cmd, buf);
    }

    GetWindowTextW(hWarpEdit, buf, sizeof(buf)/sizeof(wchar_t));
    BOOL isQuickStart = (SendMessageW(hChkQuickStart, BM_GETCHECK, 0, 0) != 0);

    if (buf[0]) {
        wcscat(cmd, L" -warp ");
        wcscat(cmd, buf);
    } else if (isQuickStart) {
        wcscat(cmd, L" -warp 1");
    }

    GetWindowTextW(hExtraEdit, buf, sizeof(buf)/sizeof(wchar_t));
    if (buf[0]) {
        wcscat(cmd, L" ");
        wcscat(cmd, buf);
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    AllowSetForegroundWindow(ASFW_ANY);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        wchar_t err[1024];
        if (current_lang == 0) {
            swprintf(err, sizeof(err)/sizeof(wchar_t), L"Не удалось запустить %ls!\nКоманда:\n%ls", exeName, cmd);
            MessageBoxW(hWnd, err, L"Ошибка запуска", MB_OK | MB_ICONERROR);
        } else {
            swprintf(err, sizeof(err)/sizeof(wchar_t), L"Failed to launch %ls!\nCommand:\n%ls", exeName, cmd);
            MessageBoxW(hWnd, err, L"Launch Error", MB_OK | MB_ICONERROR);
        }
        return;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (SendMessageW(hChkCloseStart, BM_GETCHECK, 0, 0)) {
        PostQuitMessage(0);
    }
}

static BOOL CALLBACK SetFontCallback(HWND hChild, LPARAM lp)
{
    SendMessageW(hChild, WM_SETFONT, (WPARAM)hFontMain, TRUE);
    return TRUE;
}

static void DrawDarkButton(LPDRAWITEMSTRUCT dis, const wchar_t* text, HFONT font, COLORREF normalBg, COLORREF pressedBg, COLORREF borderCol, COLORREF textCol)
{
    BOOL isDown = (dis->itemState & ODS_SELECTED);
    HBRUSH hBtnBrush = CreateSolidBrush(isDown ? pressedBg : normalBg);
    FillRect(dis->hDC, &dis->rcItem, hBtnBrush);
    DeleteObject(hBtnBrush);

    HPEN hPen = CreatePen(PS_SOLID, 1, borderCol);
    HPEN hOldPen = (HPEN)SelectObject(dis->hDC, hPen);
    SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, hOldPen);
    DeleteObject(hPen);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, textCol);
    HFONT hOldFont = (HFONT)SelectObject(dis->hDC, font);

    RECT r = dis->rcItem;
    if (isDown) { r.top += 1; r.left += 1; }
    DrawTextW(dis->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dis->hDC, hOldFont);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        hMainWnd = hWnd;

        // Header Language Toggle Button
        hBtnLang = CreateWindowW(L"BUTTON", L"[ EN ]", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 455, 18, 80, 28, hWnd, (HMENU)IDC_BTN_LANG, NULL, NULL);

        // IWAD Section
        hLblIwad = CreateWindowW(L"STATIC", L"Файл игры (IWAD):", WS_CHILD | WS_VISIBLE, 25, 75, 200, 16, hWnd, (HMENU)IDC_LBL_IWAD, NULL, NULL);
        hIwadPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 25, 95, 395, 26, hWnd, (HMENU)IDC_IWAD_PATH, NULL, NULL);
        hIwadBrowse = CreateWindowW(L"BUTTON", L"Обзор...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 430, 95, 105, 26, hWnd, (HMENU)IDC_IWAD_BROWSE, NULL, NULL);

        hLblPreset = CreateWindowW(L"STATIC", L"Обнаруженные WAD:", WS_CHILD | WS_VISIBLE, 25, 128, 140, 18, hWnd, (HMENU)IDC_LBL_PRESET, NULL, NULL);
        hIwadPreset = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 170, 125, 365, 200, hWnd, (HMENU)IDC_IWAD_PRESET, NULL, NULL);

        // PWAD Section
        hLblPwad = CreateWindowW(L"STATIC", L"Дополнительный мод / PWAD (-file):", WS_CHILD | WS_VISIBLE, 25, 159, 300, 16, hWnd, (HMENU)IDC_LBL_PWAD, NULL, NULL);
        hPwadPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 25, 179, 395, 26, hWnd, (HMENU)IDC_PWAD_PATH, NULL, NULL);
        hPwadBrowse = CreateWindowW(L"BUTTON", L"Обзор...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 430, 179, 105, 26, hWnd, (HMENU)IDC_PWAD_BROWSE, NULL, NULL);

        // Resolution & Display Section
        InitResolutions();
        hLblRes = CreateWindowW(L"STATIC", L"Разрешение экрана:", WS_CHILD | WS_VISIBLE, 25, 220, 140, 18, hWnd, (HMENU)IDC_LBL_RES, NULL, NULL);
        hResCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 170, 217, 365, 200, hWnd, (HMENU)IDC_RES_COMBO, NULL, NULL);
        for (int i = 0; i < res_count; i++)
            SendMessageW(hResCombo, CB_ADDSTRING, 0, (LPARAM)res_options[i].label);

        hChkKeepAspect = CreateWindowW(L"BUTTON", L"Сохранять пропорции (-keepaspect)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 250, 240, 20, hWnd, (HMENU)IDC_CHK_KEEPASPECT, NULL, NULL);
        hChkSmooth = CreateWindowW(L"BUTTON", L"Сглаживание (Bilinear) (-smooth)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 275, 250, 260, 20, hWnd, (HMENU)IDC_CHK_SMOOTH, NULL, NULL);
        hChkFullscreen = CreateWindowW(L"BUTTON", L"Полный экран (-fullscreen)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 275, 230, 20, hWnd, (HMENU)IDC_CHK_FULLSCREEN, NULL, NULL);
        hChkMaximized = CreateWindowW(L"BUTTON", L"Окно без рамок (-maximized)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 275, 275, 260, 20, hWnd, (HMENU)IDC_CHK_MAXIMIZED, NULL, NULL);

        // Gameplay / AI Section
        hChkBot = CreateWindowW(L"BUTTON", L"Включить бота (-bot)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 308, 230, 20, hWnd, (HMENU)IDC_CHK_BOT, NULL, NULL);
        hChkMlook = CreateWindowW(L"BUTTON", L"Обзор мышью (-mlook)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 275, 308, 260, 20, hWnd, (HMENU)IDC_CHK_MLOOK, NULL, NULL);

        hChkMenuMouse = CreateWindowW(L"BUTTON", L"Курсор мыши в меню (-menumouse)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 332, 245, 20, hWnd, (HMENU)IDC_CHK_MENUMOUSE, NULL, NULL);
        hChkFast = CreateWindowW(L"BUTTON", L"Быстрые монстры (-fast)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 275, 332, 260, 20, hWnd, (HMENU)IDC_CHK_FAST, NULL, NULL);

        hChkNoMonsters = CreateWindowW(L"BUTTON", L"Без монстров (-nomonsters)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 356, 230, 20, hWnd, (HMENU)IDC_CHK_NOMONSTERS, NULL, NULL);
        hChkRespawn = CreateWindowW(L"BUTTON", L"Возрождение монстров (-respawn)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 275, 356, 260, 20, hWnd, (HMENU)IDC_CHK_RESPAWN, NULL, NULL);

        hChkJUMP = CreateWindowW(L"BUTTON", L"Прыжок (Space) (-jump)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 380, 230, 20, hWnd, (HMENU)IDC_CHK_JUMP, NULL, NULL);
        hChkQuickStart = CreateWindowW(L"BUTTON", L"Сразу в игру (минуя заставку)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 275, 380, 260, 20, hWnd, (HMENU)IDC_CHK_QUICKSTART, NULL, NULL);

        // Difficulty & Warp
        hLblSkill = CreateWindowW(L"STATIC", L"Сложность:", WS_CHILD | WS_VISIBLE, 25, 412, 80, 18, hWnd, (HMENU)IDC_LBL_SKILL, NULL, NULL);
        hSkillCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 110, 409, 245, 180, hWnd, (HMENU)IDC_SKILL_COMBO, NULL, NULL);
        for (int i = 0; i < 5; i++)
            SendMessageW(hSkillCombo, CB_ADDSTRING, 0, (LPARAM)skill_list_ru[i]);

        hLblWarp = CreateWindowW(L"STATIC", L"Карта (Warp):", WS_CHILD | WS_VISIBLE, 370, 412, 95, 18, hWnd, (HMENU)IDC_LBL_WARP, NULL, NULL);
        hWarpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 470, 409, 65, 24, hWnd, (HMENU)IDC_WARP_EDIT, NULL, NULL);

        // Extra args
        hLblExtra = CreateWindowW(L"STATIC", L"Дополнительные параметры:", WS_CHILD | WS_VISIBLE, 25, 442, 250, 18, hWnd, (HMENU)IDC_LBL_EXTRA, NULL, NULL);
        hExtraEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 25, 462, 510, 24, hWnd, (HMENU)IDC_EXTRA_EDIT, NULL, NULL);

        hChkCloseStart = CreateWindowW(L"BUTTON", L"Закрывать ланчер при запуске игры", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 494, 350, 20, hWnd, (HMENU)IDC_CHK_CLOSE_START, NULL, NULL);

        // Buttons
        hBtnLaunch = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 25, 522, 510, 46, hWnd, (HMENU)IDC_BTN_LAUNCH, NULL, NULL);
        hBtnSave = CreateWindowW(L"BUTTON", L"Сохранить настройки", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 25, 576, 245, 30, hWnd, (HMENU)IDC_BTN_SAVE, NULL, NULL);
        hBtnExit = CreateWindowW(L"BUTTON", L"Выход", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 290, 576, 245, 30, hWnd, (HMENU)IDC_BTN_EXIT, NULL, NULL);

        EnumChildWindows(hWnd, SetFontCallback, 0);

        LoadSettings();
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_BTN_LANG) {
            current_lang = 1 - current_lang;
            ApplyLanguage(current_lang);
            SaveSettings();
        }
        else if (id == IDC_IWAD_PRESET && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(hIwadPreset, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < found_wad_count) {
                SetWindowTextW(hIwadPath, found_wads[sel].path);
            }
        }
        else if (id == IDC_IWAD_BROWSE) {
            BrowseFile(hWnd, hIwadPath, current_lang == 0 ? L"Выберите файл игры (IWAD)" : L"Select Game IWAD File");
        }
        else if (id == IDC_PWAD_BROWSE) {
            BrowseFile(hWnd, hPwadPath, current_lang == 0 ? L"Выберите файл мода (PWAD)" : L"Select Mod PWAD File");
        }
        else if (id == IDC_BTN_LAUNCH) {
            LaunchGame(hWnd);
        }
        else if (id == IDC_BTN_SAVE) {
            SaveSettings();
            if (current_lang == 0)
                MessageBoxW(hWnd, L"Настройки успешно сохранены в launcher.ini!", L"Сохранено", MB_OK | MB_ICONINFORMATION);
            else
                MessageBoxW(hWnd, L"Settings saved successfully to launcher.ini!", L"Saved", MB_OK | MB_ICONINFORMATION);
        }
        else if (id == IDC_BTN_EXIT) {
            PostQuitMessage(0);
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, colBg);
        SetTextColor(hdc, colText);
        return (LRESULT)hBrushBg;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, colPanel);
        SetTextColor(hdc, RGB(255, 255, 255));
        return (LRESULT)hBrushPanel;
    }

    case WM_CTLCOLORBTN: {
        return (LRESULT)hBrushBg;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlID == IDC_BTN_LAUNCH) {
            BOOL isDown = (dis->itemState & ODS_SELECTED);
            HBRUSH hBtnBrush = CreateSolidBrush(isDown ? RGB(140, 18, 18) : RGB(185, 25, 25));
            FillRect(dis->hDC, &dis->rcItem, hBtnBrush);
            DeleteObject(hBtnBrush);

            HPEN hPen = CreatePen(PS_SOLID, 2, isDown ? RGB(180, 50, 50) : RGB(225, 65, 65));
            HPEN hOldPen = (HPEN)SelectObject(dis->hDC, hPen);
            SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, hOldPen);
            DeleteObject(hPen);

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, RGB(255, 255, 255));
            HFONT hOldFont = (HFONT)SelectObject(dis->hDC, hFontLaunch);

            RECT r = dis->rcItem;
            if (isDown) { r.top += 1; r.left += 1; }
            const wchar_t *launchText = (current_lang == 0) ? L"►  ЗАПУСТИТЬ ANTIDOOM" : L"►  LAUNCH ANTIDOOM";
            DrawTextW(dis->hDC, launchText, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(dis->hDC, hOldFont);
            return TRUE;
        }
        else if (dis->CtlID == IDC_BTN_LANG) {
            const wchar_t *langText = (current_lang == 0) ? L"EN" : L"RU";
            DrawDarkButton(dis, langText, hFontBtn, RGB(55, 45, 80), RGB(35, 28, 55), colGold, colGold);
            return TRUE;
        }
        else if (dis->CtlID == IDC_BTN_SAVE) {
            const wchar_t *saveText = (current_lang == 0) ? L"Сохранить настройки" : L"Save Settings";
            DrawDarkButton(dis, saveText, hFontBtn, RGB(44, 48, 58), RGB(30, 33, 40), RGB(75, 82, 98), RGB(235, 238, 245));
            return TRUE;
        }
        else if (dis->CtlID == IDC_BTN_EXIT) {
            const wchar_t *exitText = (current_lang == 0) ? L"Выход" : L"Exit";
            DrawDarkButton(dis, exitText, hFontBtn, RGB(44, 48, 58), RGB(30, 33, 40), RGB(75, 82, 98), RGB(235, 238, 245));
            return TRUE;
        }
        else if (dis->CtlID == IDC_IWAD_BROWSE || dis->CtlID == IDC_PWAD_BROWSE) {
            const wchar_t *brText = (current_lang == 0) ? L"Обзор..." : L"Browse...";
            DrawDarkButton(dis, brText, hFontBtn, RGB(46, 50, 60), RGB(32, 35, 42), RGB(80, 88, 104), RGB(235, 238, 245));
            return TRUE;
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        // Header Background
        RECT rHeader = {0, 0, 560, 65};
        HBRUSH hHeaderBrush = CreateSolidBrush(RGB(18, 20, 24));
        FillRect(hdc, &rHeader, hHeaderBrush);
        DeleteObject(hHeaderBrush);

        // Header separator line
        HPEN hGoldPen = CreatePen(PS_SOLID, 2, colGold);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hGoldPen);
        MoveToEx(hdc, 0, 65, NULL);
        LineTo(hdc, 560, 65);
        SelectObject(hdc, hOldPen);
        DeleteObject(hGoldPen);

        // Header Texts
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, colGold);
        HFONT hOld = (HFONT)SelectObject(hdc, hFontTitle);
        TextOutW(hdc, 25, 10, L"AntiDoom : Win32 Launcher", 26);

        SetTextColor(hdc, colGray);
        SelectObject(hdc, hFontSub);
        if (current_lang == 0) {
            TextOutW(hdc, 26, 38, L"Управление параметрами запуска, модами и ботом", 46);
        } else {
            TextOutW(hdc, 26, 38, L"Game launch settings, PWAD mods, and bot control", 48);
        }

        SelectObject(hdc, hOld);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    wchar_t launcherDir[MAX_PATH] = {0};
    GetLauncherDir(launcherDir, MAX_PATH);
    if (launcherDir[0]) SetCurrentDirectoryW(launcherDir);

    hBrushBg = CreateSolidBrush(colBg);
    hBrushPanel = CreateSolidBrush(colPanel);
    hBrushEdit = CreateSolidBrush(colPanel);

    hFontTitle = CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    hFontSub = CreateFontW(14, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    hFontMain = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    hFontBold = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    hFontBtn = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    hFontLaunch = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"AntiDoomLauncher";
    wc.hbrBackground = hBrushBg;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);

    if (!RegisterClassW(&wc)) {
        CoUninitialize();
        return 1;
    }

    RECT rc = {0, 0, 560, 625};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int posX = (scrW - winW) / 2;
    int posY = (scrH - winH) / 2;

    HWND hWnd = CreateWindowW(L"AntiDoomLauncher", L"AntiDoom - Win32 Launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        posX, posY, winW, winH, NULL, NULL, hInstance, NULL);

    if (!hWnd) {
        CoUninitialize();
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            LaunchGame(hWnd);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(hFontTitle);
    DeleteObject(hFontSub);
    DeleteObject(hFontMain);
    DeleteObject(hFontBold);
    DeleteObject(hFontBtn);
    DeleteObject(hFontLaunch);
    DeleteObject(hBrushBg);
    DeleteObject(hBrushPanel);
    DeleteObject(hBrushEdit);

    CoUninitialize();
    return (int)msg.wParam;
}
