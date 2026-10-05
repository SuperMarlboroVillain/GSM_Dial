/**
 ****************************************************************************************************
 * @file        gsm_ui.c
 * @brief       emWin multi-page UI (480x800 portrait):
 *                page 0 DIAL   - arbitrary phone number dialing, hang up, answer
 *                page 1 SMS    - arbitrary number SMS sending
 *                page 2 INBOX  - received / sent SMS list with details
 *              plus a bottom status bar (module state, signal, call state, help).
 *
 *              Only the owning task (user_task) calls into this module; the GSM
 *              engine runs in its own task and communicates through gsm.h state.
 ****************************************************************************************************
 */

#include "gsm_ui.h"
#include "gsm.h"
#include "GUI.h"
#include "WM.h"
#include "DIALOG.h"
#include "cpu.h"
#include "os.h"

#include <stdio.h>
#include <string.h>

/******************************************************************************************/
/* Widget IDs: main dialog */

#define ID_FRAME        (GUI_ID_USER + 0x00)
#define ID_MP           (GUI_ID_USER + 0x01)
#define ID_TEXT_STAT    (GUI_ID_USER + 0x02)
#define ID_TEXT_HINT    (GUI_ID_USER + 0x03)

/* Dial page */
#define ID_D_EDIT       (GUI_ID_USER + 0x10)
#define ID_D_DEL        (GUI_ID_USER + 0x11)
#define ID_D_CALL       (GUI_ID_USER + 0x12)
#define ID_D_END        (GUI_ID_USER + 0x13)
#define ID_D_ANS        (GUI_ID_USER + 0x14)
#define ID_D_INFO       (GUI_ID_USER + 0x15)
#define ID_D_KEY1       (GUI_ID_USER + 0x16)
#define ID_D_KEY2       (GUI_ID_USER + 0x17)
#define ID_D_KEY3       (GUI_ID_USER + 0x18)
#define ID_D_KEY4       (GUI_ID_USER + 0x19)
#define ID_D_KEY5       (GUI_ID_USER + 0x1A)
#define ID_D_KEY6       (GUI_ID_USER + 0x1B)
#define ID_D_KEY7       (GUI_ID_USER + 0x1C)
#define ID_D_KEY8       (GUI_ID_USER + 0x1D)
#define ID_D_KEY9       (GUI_ID_USER + 0x1E)
#define ID_D_KEYSTAR    (GUI_ID_USER + 0x1F)
#define ID_D_KEY0       (GUI_ID_USER + 0x20)
#define ID_D_KEYSHARP   (GUI_ID_USER + 0x21)

/* SMS page */
#define ID_S_NUM        (GUI_ID_USER + 0x30)
#define ID_S_DEL        (GUI_ID_USER + 0x31)
#define ID_S_MSG        (GUI_ID_USER + 0x32)
#define ID_S_SEND       (GUI_ID_USER + 0x33)
#define ID_S_CLEAR      (GUI_ID_USER + 0x34)
#define ID_S_RESULT     (GUI_ID_USER + 0x35)

/* SMS on-screen keyboard (ABC group + 123 group, toggled) */
#define ID_KB_A         (GUI_ID_USER + 0x90)      /* ID_KB_A .. ID_KB_A+25 = A..Z */
#define ID_KB_1         (GUI_ID_USER + 0xB0)      /* ID_KB_1 .. ID_KB_1+9  = 1..0 */
#define ID_KB_STAR      (GUI_ID_USER + 0xC0)
#define ID_KB_SHARP     (GUI_ID_USER + 0xC1)
#define ID_KB_MINUS     (GUI_ID_USER + 0xC2)
#define ID_KB_DOT       (GUI_ID_USER + 0xC3)
#define ID_KB_COMMA     (GUI_ID_USER + 0xC4)
#define ID_KB_PLUS      (GUI_ID_USER + 0xC5)
#define ID_KB_QM        (GUI_ID_USER + 0xC6)
#define ID_KB_EX        (GUI_ID_USER + 0xC7)
#define ID_KB_MODE      (GUI_ID_USER + 0xC8)      /* "123" toggle, ABC group */
#define ID_KB_SPACE_A   (GUI_ID_USER + 0xC9)
#define ID_KB_BKSP_A    (GUI_ID_USER + 0xCA)
#define ID_KB_MODE_N    (GUI_ID_USER + 0xCB)      /* "ABC" toggle, 123 group */
#define ID_KB_SPACE_N   (GUI_ID_USER + 0xCC)
#define ID_KB_BKSP_N    (GUI_ID_USER + 0xCD)

/* Inbox page */
#define ID_I_LIST       (GUI_ID_USER + 0x50)
#define ID_I_MSG        (GUI_ID_USER + 0x51)

/* AT test page */
#define ID_A_EDIT       (GUI_ID_USER + 0x60)
#define ID_A_DEL        (GUI_ID_USER + 0x61)
#define ID_A_SEND       (GUI_ID_USER + 0x62)
#define ID_A_CSQ        (GUI_ID_USER + 0x63)
#define ID_A_CLEAR      (GUI_ID_USER + 0x64)
#define ID_A_SIG        (GUI_ID_USER + 0x65)
#define ID_A_LOG        (GUI_ID_USER + 0x66)
#define ID_A_AT         (GUI_ID_USER + 0x67)
#define ID_A_REG        (GUI_ID_USER + 0x68)

/* Incoming-call popup */
#define ID_P_TITLE      (GUI_ID_USER + 0x70)
#define ID_P_NUM        (GUI_ID_USER + 0x71)
#define ID_P_ANS        (GUI_ID_USER + 0x72)
#define ID_P_REJ        (GUI_ID_USER + 0x73)

/* New-SMS popup */
#define ID_N_TITLE      (GUI_ID_USER + 0x80)
#define ID_N_FROM       (GUI_ID_USER + 0x81)
#define ID_N_MSG        (GUI_ID_USER + 0x82)
#define ID_N_VIEW       (GUI_ID_USER + 0x83)
#define ID_N_CLOSE      (GUI_ID_USER + 0x84)

/* Static page labels (styled text) */
#define ID_D_LABEL      (GUI_ID_USER + 0x22)      /* "NUMBER" on the dial page  */
#define ID_S_LABEL1     (GUI_ID_USER + 0x36)      /* "TO:"    on the SMS page   */
#define ID_S_LABEL2     (GUI_ID_USER + 0x37)      /* "MSG:"   on the SMS page   */
#define ID_A_LABEL      (GUI_ID_USER + 0x69)      /* "AT CMD:" on the AT page   */

/******************************************************************************************/
/* Color palette (dark theme) */

#define UI_COL_BG        GUI_BLACK        /* page / desktop background   */
#define UI_COL_CARD      GUI_WHITE        /* input fields / lists        */
#define UI_COL_TEXT      GUI_WHITE        /* primary text on dark bg     */
#define UI_COL_SUBTEXT   GUI_LIGHTGRAY    /* secondary text on dark bg   */
#define UI_COL_KEY       GUI_LIGHTGRAY    /* keypad buttons              */
#define UI_COL_KEYTEXT   GUI_BLACK        /* keypad button text          */
#define UI_COL_FUNC      GUI_GRAY         /* DEL / BKSP / CLEAR buttons  */
#define UI_COL_OK        GUI_DARKGREEN    /* CALL / SEND / ANSWER / VIEW */
#define UI_COL_NO        GUI_RED          /* END / REJECT                */
#define UI_COL_ACCENT    GUI_DARKBLUE     /* AT presets / ANSW           */
#define UI_COL_STATBAR   GUI_DARKBLUE     /* bottom status bar           */

/******************************************************************************************/
/* Widget creation tables */

static const GUI_WIDGET_CREATE_INFO _aMainCreate[] = {
    { FRAMEWIN_CreateIndirect,      "GSM",      ID_FRAME,       0,   0, 480, 800, 0, 0x0, 0 },
    { MULTIPAGE_CreateIndirect,     "MP",       ID_MP,          0,   0, 480, 716, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_TEXT_STAT,   6, 722, 468,  26, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_TEXT_HINT,   6, 752, 468,  24, 0, 0x0, 0 },
};

static const GUI_WIDGET_CREATE_INFO _aDialCreate[] = {
    { WINDOW_CreateIndirect,        "Dial",     0,              0,   0, 480, 690, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "NUMBER",   ID_D_LABEL,    10,   8, 140,  24, 0, 0x0, 0 },
    { EDIT_CreateIndirect,          "",         ID_D_EDIT,     10,  36, 360,  44, 0, 0x64, 0 },
    { BUTTON_CreateIndirect,        "DEL",      ID_D_DEL,     380,  36,  90,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "1",        ID_D_KEY1,     30,  96, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "2",        ID_D_KEY2,    175,  96, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "3",        ID_D_KEY3,    320,  96, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "4",        ID_D_KEY4,     30, 172, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "5",        ID_D_KEY5,    175, 172, 130, 64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "6",        ID_D_KEY6,    320, 172, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "7",        ID_D_KEY7,     30, 248, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "8",        ID_D_KEY8,    175, 248, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "9",        ID_D_KEY9,    320, 248, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "*",        ID_D_KEYSTAR,   30, 324, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "0",        ID_D_KEY0,    175, 324, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "#",        ID_D_KEYSHARP, 320, 324, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "CALL",     ID_D_CALL,     30, 404, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "END",      ID_D_END,     175, 404, 130,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "ANSW",     ID_D_ANS,     320, 404, 130,  64, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_D_INFO,     10, 484, 460, 130, 0, 0x0, 0 },
};

static const GUI_WIDGET_CREATE_INFO _aSmsCreate[] = {
    { WINDOW_CreateIndirect,        "Sms",      0,              0,   0, 480, 690, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "TO:",      ID_S_LABEL1,   10,   8, 140,  24, 0, 0x0, 0 },
    { EDIT_CreateIndirect,          "",         ID_S_NUM,      10,  32, 340,  40, 0, 0x64, 0 },
    { BUTTON_CreateIndirect,        "DEL",      ID_S_DEL,     360,  32, 110,  40, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "MSG:",     ID_S_LABEL2,   10,  84, 140,  24, 0, 0x0, 0 },
    { MULTIEDIT_CreateIndirect,     "",         ID_S_MSG,      10, 108, 460, 190, 0, 0x0, 0 },
    /* ABC keyboard */
    { BUTTON_CreateIndirect,        "Q",        ID_KB_A + 0,   12, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "W",        ID_KB_A + 1,   58, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "E",        ID_KB_A + 2,  104, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "R",        ID_KB_A + 3,  150, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "T",        ID_KB_A + 4,  196, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "Y",        ID_KB_A + 5,  242, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "U",        ID_KB_A + 6,  288, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "I",        ID_KB_A + 7,  334, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "O",        ID_KB_A + 8,  380, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "P",        ID_KB_A + 9,  426, 310,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "A",        ID_KB_A + 10,  17, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "S",        ID_KB_A + 11,  67, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "D",        ID_KB_A + 12, 117, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "F",        ID_KB_A + 13, 167, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "G",        ID_KB_A + 14, 217, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "H",        ID_KB_A + 15, 267, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "J",        ID_KB_A + 16, 317, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "K",        ID_KB_A + 17, 367, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "L",        ID_KB_A + 18, 417, 362,  46,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "Z",        ID_KB_A + 19,  18, 414,  52,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "X",        ID_KB_A + 20,  74, 414,  52,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "C",        ID_KB_A + 21, 130, 414,  52,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "V",        ID_KB_A + 22, 186, 414,  52,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "B",        ID_KB_A + 23, 242, 414,  52,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "N",        ID_KB_A + 24, 298, 414,  52,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "M",        ID_KB_A + 25, 354, 414,  52,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "DEL",      ID_KB_BKSP_A, 410, 414,  60,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "123",      ID_KB_MODE,    10, 466,  80,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "SPACE",    ID_KB_SPACE_A,100, 466, 270,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        ".",        ID_KB_DOT,    380, 466,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        ",",        ID_KB_COMMA,  428, 466,  42,  44, 0, 0x0, 0 },
    /* 123 keyboard (hidden until toggled) */
    { BUTTON_CreateIndirect,        "1",        ID_KB_1 + 0,   12, 310,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "2",        ID_KB_1 + 1,  104, 310,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "3",        ID_KB_1 + 2,  196, 310,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "4",        ID_KB_1 + 3,  288, 310,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "5",        ID_KB_1 + 4,  380, 310,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "6",        ID_KB_1 + 5,   12, 362,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "7",        ID_KB_1 + 6,  104, 362,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "8",        ID_KB_1 + 7,  196, 362,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "9",        ID_KB_1 + 8,  288, 362,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "0",        ID_KB_1 + 9,  380, 362,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "*",        ID_KB_STAR,    12, 414,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "#",        ID_KB_SHARP,  104, 414,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "-",        ID_KB_MINUS,  196, 414,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "+",        ID_KB_PLUS,   288, 414,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "DEL",      ID_KB_BKSP_N, 380, 414,  86,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "ABC",      ID_KB_MODE_N,  10, 466, 110,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "SPACE",    ID_KB_SPACE_N,130, 466, 240,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "?",        ID_KB_QM,     380, 466,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "!",        ID_KB_EX,     428, 466,  42,  44, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "SEND",     ID_S_SEND,     10, 522, 220,  56, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "CLEAR",    ID_S_CLEAR,   250, 522, 220,  56, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_S_RESULT,   10, 586, 460,  28, 0, 0x0, 0 },
};

static const GUI_WIDGET_CREATE_INFO _aInboxCreate[] = {
    { WINDOW_CreateIndirect,        "Inbox",    0,              0,   0, 480, 690, 0, 0x0, 0 },
    { LISTBOX_CreateIndirect,       "",         ID_I_LIST,     10,  10, 460, 420, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_I_MSG,      10, 440, 460, 230, 0, 0x0, 0 },
};

static const GUI_WIDGET_CREATE_INFO _aAtCreate[] = {
    { WINDOW_CreateIndirect,        "At",       0,              0,   0, 480, 690, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "AT CMD:",  ID_A_LABEL,    10,   8, 200,  24, 0, 0x0, 0 },
    { EDIT_CreateIndirect,          "",         ID_A_EDIT,     10,  32, 260,  40, 0, 0x64, 0 },
    { BUTTON_CreateIndirect,        "DEL",      ID_A_DEL,     280,  32,  90,  40, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "CLRLOG",   ID_A_CLEAR,   380,  32,  90,  40, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "SEND",     ID_A_SEND,     10,  84, 110,  48, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "AT",       ID_A_AT,      130,  84, 100,  48, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "CSQ",      ID_A_CSQ,     240,  84, 100,  48, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "CREG",     ID_A_REG,     350,  84, 120,  48, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_A_SIG,      10, 140, 460,  26, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_A_LOG,      10, 172, 460, 500, 0, 0x0, 0 },
};

static const GUI_WIDGET_CREATE_INFO _aCallPopupCreate[] = {
    { WINDOW_CreateIndirect,        "CallPop",  0,             40, 270, 400, 230, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "INCOMING CALL", ID_P_TITLE, 0,  18, 400,  32, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_P_NUM,       0,  58, 400,  36, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "ANSWER",   ID_P_ANS,      40, 130, 150,  64, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "REJECT",   ID_P_REJ,     210, 130, 150,  64, 0, 0x0, 0 },
};

static const GUI_WIDGET_CREATE_INFO _aSmsPopupCreate[] = {
    { WINDOW_CreateIndirect,        "SmsPop",   0,             40,  80, 400, 230, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "NEW SMS",  ID_N_TITLE,     0,  14, 400,  32, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_N_FROM,      0,  50, 400,  24, 0, 0x0, 0 },
    { TEXT_CreateIndirect,          "",         ID_N_MSG,      16,  80, 368,  70, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "VIEW",     ID_N_VIEW,     40, 160, 150,  56, 0, 0x0, 0 },
    { BUTTON_CreateIndirect,        "CLOSE",    ID_N_CLOSE,   210, 160, 150,  56, 0, 0x0, 0 },
};

/******************************************************************************************/
/* Handles and caches (only touched by the owning task) */

static WM_HWIN s_hStat;
static WM_HWIN s_hHint;
static WM_HWIN s_hMp;
static WM_HWIN s_hDialEdit;
static WM_HWIN s_hCallInfo;
static WM_HWIN s_hSmsNum;
static WM_HWIN s_hSmsMsg;
static WM_HWIN s_hSmsResult;
static WM_HWIN s_hInboxList;
static WM_HWIN s_hInboxMsg;
static WM_HWIN s_hAtEdit;
static WM_HWIN s_hAtSig;
static WM_HWIN s_hAtLog;

/* popups are created lazily on first use */
static WM_HWIN s_hCallPopup = 0;
static WM_HWIN s_hCallPopupNum = 0;
static WM_HWIN s_hSmsPopup = 0;
static WM_HWIN s_hSmsPopupFrom = 0;
static WM_HWIN s_hSmsPopupMsg = 0;

static char     s_stat_cache[80];
static char     s_call_cache[120];
static char     s_sig_cache[48];
static char     s_popup_num_cache[24];
static uint8_t  s_result_cache = 0xFF;
static uint32_t s_inbox_cache = 0xFFFFFFFF;
static uint32_t s_atlog_cache = 0xFFFFFFFF;
static uint32_t s_inbox_seen = 0;               /* inbox counter seen by the UI */
static char     s_logbuf[2100];                 /* AT log copy buffer */

/******************************************************************************************/
/* Look and feel */

/**
 * @brief       Global visual style: dark desktop, flat classic-button skin so
 *              per-button colors take effect (the flex skin ignores them).
 */
static void ui_style_init(void)
{
    GUI_SetBkColor(UI_COL_BG);
    GUI_SetColor(UI_COL_TEXT);
    GUI_Clear();
    BUTTON_SetDefaultSkinClassic();
    WIDGET_SetDefaultEffect(&WIDGET_Effect_None);
}

/**
 * @brief       Style one button: font, flat background, text color
 *              (pressed state looks the same as unpressed).
 */
static void btn_style(WM_HWIN hBtn, const GUI_FONT *font, GUI_COLOR bg, GUI_COLOR fg)
{
    if (hBtn == 0)
    {
        return;
    }
    BUTTON_SetFont(hBtn, font);
    BUTTON_SetBkColor(hBtn, BUTTON_CI_UNPRESSED, bg);
    BUTTON_SetBkColor(hBtn, BUTTON_CI_PRESSED, bg);
    BUTTON_SetTextColor(hBtn, BUTTON_CI_UNPRESSED, fg);
    BUTTON_SetTextColor(hBtn, BUTTON_CI_PRESSED, fg);
}

/**
 * @brief       Style a static label TEXT.
 */
static void lbl_style(WM_HWIN hPage, int id, const char *reset_txt)
{
    WM_HWIN h = WM_GetDialogItem(hPage, id);

    if (h == 0)
    {
        return;
    }
    TEXT_SetFont(h, &GUI_Font16B_ASCII);
    TEXT_SetTextColor(h, UI_COL_SUBTEXT);
    if (reset_txt != NULL)
    {
        TEXT_SetText(h, reset_txt);
    }
}

/******************************************************************************************/
/* Small helpers */

/**
 * @brief       Insert a newline every 'width' chars so long texts fit the detail area.
 */
static void wrap_text(char *dst, int dst_max, const char *src, int width)
{
    int i = 0, col = 0;

    while (*src && i < dst_max - 2)
    {
        if (col >= width)
        {
            dst[i++] = '\n';
            col = 0;
        }
        dst[i++] = *src++;
        col++;
    }
    dst[i] = 0;
}

/**
 * @brief       Rebuild the inbox listbox from the shared inbox, newest first.
 */
static void inbox_refresh(void)
{
    char  item[80];
    char  detail[200];
    char  body[170];
    int   cnt;
    int   i;

    LISTBOX_SetFont(s_hInboxList, &GUI_Font16B_ASCII);
    while (LISTBOX_GetNumItems(s_hInboxList) > 0)
    {
        LISTBOX_DeleteItem(s_hInboxList, 0);
    }

    cnt = (int)g_gsm_inbox_cnt;
    if (cnt > GSM_INBOX_MAX)
    {
        cnt = GSM_INBOX_MAX;
    }

    if (cnt == 0)
    {
        LISTBOX_AddString(s_hInboxList, "(no SMS yet)");
        TEXT_SetText(s_hInboxMsg, "received and sent SMS show up here");
        return;
    }

    for (i = 0; i < cnt; i++)
    {
        int slot = (int)((g_gsm_inbox_cnt - 1 - (uint32_t)i) % GSM_INBOX_MAX);
        snprintf(item, sizeof(item), "%d.%s: %.32s",
                 i + 1, g_gsm_inbox[slot].num, g_gsm_inbox[slot].msg);
        LISTBOX_AddString(s_hInboxList, item);
    }

    LISTBOX_SetSel(s_hInboxList, 0);

    /* newest message in the detail area */
    {
        int slot = (int)((g_gsm_inbox_cnt - 1) % GSM_INBOX_MAX);
        wrap_text(body, sizeof(body), g_gsm_inbox[slot].msg, 44);
        snprintf(detail, sizeof(detail), "%s\n%s", g_gsm_inbox[slot].num, body);
        TEXT_SetText(s_hInboxMsg, detail);
    }
}

/**
 * @brief       Show the selected inbox entry in the detail area.
 */
static void inbox_show_sel(void)
{
    int sel = LISTBOX_GetSel(s_hInboxList);
    int cnt = (int)g_gsm_inbox_cnt;
    char detail[200];
    char body[170];
    int slot;

    if (cnt > GSM_INBOX_MAX)
    {
        cnt = GSM_INBOX_MAX;
    }
    if (sel < 0 || sel >= cnt)
    {
        return;
    }

    slot = (int)((g_gsm_inbox_cnt - 1 - (uint32_t)sel) % GSM_INBOX_MAX);
    wrap_text(body, sizeof(body), g_gsm_inbox[slot].msg, 44);
    snprintf(detail, sizeof(detail), "%s\n%s", g_gsm_inbox[slot].num, body);
    TEXT_SetText(s_hInboxMsg, detail);
}

/******************************************************************************************/
/* Dial page callback */

static void _cbDial(WM_MESSAGE *pMsg)
{
    WM_HWIN hItem;
    int     Id;
    int     NCode;

    switch (pMsg->MsgId)
    {
    case WM_INIT_DIALOG:
        WINDOW_SetBkColor(pMsg->hWin, UI_COL_BG);
        lbl_style(pMsg->hWin, ID_D_LABEL, NULL);

        hItem = WM_GetDialogItem(pMsg->hWin, ID_D_EDIT);
        EDIT_SetFont(hItem, GUI_FONT_32B_ASCII);
        EDIT_SetMaxLen(hItem, GSM_NUM_MAX_LEN);
        EDIT_SetBkColor(hItem, EDIT_CI_ENABLED, UI_COL_CARD);
        EDIT_SetTextColor(hItem, EDIT_CI_ENABLED, GUI_BLACK);
        s_hDialEdit = hItem;

        /* keypad digits: big font on light keys */
        {
            int i;
            for (i = 0; i < 12; i++)
            {
                btn_style(WM_GetDialogItem(pMsg->hWin, ID_D_KEY1 + i),
                          GUI_FONT_32B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            }
        }

        hItem = WM_GetDialogItem(pMsg->hWin, ID_D_INFO);
        TEXT_SetFont(hItem, &GUI_Font16B_ASCII);
        TEXT_SetTextColor(hItem, UI_COL_SUBTEXT);
        TEXT_SetText(hItem, "CALL: READY");
        s_hCallInfo = hItem;

        btn_style(WM_GetDialogItem(pMsg->hWin, ID_D_DEL),
                  &GUI_Font16B_ASCII, UI_COL_FUNC, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_D_CALL),
                  GUI_FONT_24B_ASCII, UI_COL_OK, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_D_END),
                  GUI_FONT_24B_ASCII, UI_COL_NO, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_D_ANS),
                  GUI_FONT_24B_ASCII, UI_COL_ACCENT, GUI_WHITE);
        break;

    case WM_NOTIFY_PARENT:
        Id    = WM_GetId(pMsg->hWinSrc);
        NCode = pMsg->Data.v;
        if (NCode != WM_NOTIFICATION_CLICKED)
        {
            break;
        }
        switch (Id)
        {
        case ID_D_DEL:
            EDIT_AddKey(s_hDialEdit, GUI_KEY_BACKSPACE);
            break;
        case ID_D_CALL:
            {
                char num[GSM_NUM_MAX_LEN + 1];
                EDIT_GetText(s_hDialEdit, num, sizeof(num));
                if (strlen(num) > 0)
                {
                    gsm_req_dial(num);
                }
            }
            break;
        case ID_D_END:
            gsm_req_hangup();
            break;
        case ID_D_ANS:
            gsm_req_answer();
            break;
        case ID_D_KEY1:     EDIT_AddKey(s_hDialEdit, '1');  break;
        case ID_D_KEY2:     EDIT_AddKey(s_hDialEdit, '2');  break;
        case ID_D_KEY3:     EDIT_AddKey(s_hDialEdit, '3');  break;
        case ID_D_KEY4:     EDIT_AddKey(s_hDialEdit, '4');  break;
        case ID_D_KEY5:     EDIT_AddKey(s_hDialEdit, '5');  break;
        case ID_D_KEY6:     EDIT_AddKey(s_hDialEdit, '6');  break;
        case ID_D_KEY7:     EDIT_AddKey(s_hDialEdit, '7');  break;
        case ID_D_KEY8:     EDIT_AddKey(s_hDialEdit, '8');  break;
        case ID_D_KEY9:     EDIT_AddKey(s_hDialEdit, '9');  break;
        case ID_D_KEYSTAR:  EDIT_AddKey(s_hDialEdit, '*');  break;
        case ID_D_KEY0:     EDIT_AddKey(s_hDialEdit, '0');  break;
        case ID_D_KEYSHARP: EDIT_AddKey(s_hDialEdit, '#');  break;
        default:
            break;
        }
        break;

    default:
        WM_DefaultProc(pMsg);
        break;
    }
}

/******************************************************************************************/
/* SMS page keyboard helpers */

static int  s_kb_target_msg = 1;    /* 1: keys go to MSG field, 0: to NUM field */

/**
 * @brief       Send one key to the currently selected input field.
 */
static void kb_send(int ch)
{
    if (s_kb_target_msg)
    {
        MULTIEDIT_AddKey(s_hSmsMsg, ch);
    }
    else
    {
        EDIT_AddKey(s_hSmsNum, ch);
    }
}

/**
 * @brief       Show/hide one keyboard widget of the page.
 */
static void kb_vis(WM_HWIN hPage, int id, int show)
{
    WM_HWIN h = WM_GetDialogItem(hPage, id);

    if (h == 0)
    {
        return;
    }
    if (show)
    {
        WM_ShowWindow(h);
    }
    else
    {
        WM_HideWindow(h);
    }
}

/**
 * @brief       Switch between the ABC and the 123 keyboard groups.
 */
static void kb_apply_mode(WM_HWIN hPage, int numeric)
{
    int i;

    for (i = 0; i < 26; i++)
    {
        kb_vis(hPage, ID_KB_A + i, !numeric);
    }
    kb_vis(hPage, ID_KB_MODE,    !numeric);
    kb_vis(hPage, ID_KB_SPACE_A, !numeric);
    kb_vis(hPage, ID_KB_BKSP_A,  !numeric);
    kb_vis(hPage, ID_KB_DOT,     !numeric);
    kb_vis(hPage, ID_KB_COMMA,   !numeric);

    for (i = 0; i < 10; i++)
    {
        kb_vis(hPage, ID_KB_1 + i, numeric);
    }
    kb_vis(hPage, ID_KB_STAR,    numeric);
    kb_vis(hPage, ID_KB_SHARP,   numeric);
    kb_vis(hPage, ID_KB_MINUS,   numeric);
    kb_vis(hPage, ID_KB_PLUS,    numeric);
    kb_vis(hPage, ID_KB_QM,      numeric);
    kb_vis(hPage, ID_KB_EX,      numeric);
    kb_vis(hPage, ID_KB_MODE_N,  numeric);
    kb_vis(hPage, ID_KB_SPACE_N, numeric);
    kb_vis(hPage, ID_KB_BKSP_N,  numeric);
}

/******************************************************************************************/
/* SMS page callback */

static void _cbSms(WM_MESSAGE *pMsg)
{
    WM_HWIN hItem;
    int     Id;
    int     NCode;

    switch (pMsg->MsgId)
    {
    case WM_INIT_DIALOG:
        WINDOW_SetBkColor(pMsg->hWin, UI_COL_BG);
        lbl_style(pMsg->hWin, ID_S_LABEL1, NULL);
        lbl_style(pMsg->hWin, ID_S_LABEL2, NULL);

        hItem = WM_GetDialogItem(pMsg->hWin, ID_S_NUM);
        EDIT_SetFont(hItem, GUI_FONT_24B_ASCII);
        EDIT_SetMaxLen(hItem, GSM_NUM_MAX_LEN);
        EDIT_SetBkColor(hItem, EDIT_CI_ENABLED, UI_COL_CARD);
        EDIT_SetTextColor(hItem, EDIT_CI_ENABLED, GUI_BLACK);
        s_hSmsNum = hItem;

        hItem = WM_GetDialogItem(pMsg->hWin, ID_S_MSG);
        MULTIEDIT_SetFont(hItem, &GUI_Font16B_ASCII);
        MULTIEDIT_SetMaxNumChars(hItem, GSM_SMS_MAX_LEN);
        MULTIEDIT_SetAutoScrollH(hItem, 1);
        MULTIEDIT_SetBkColor(hItem, MULTIEDIT_CI_EDIT, UI_COL_CARD);
        MULTIEDIT_SetTextColor(hItem, MULTIEDIT_CI_EDIT, GUI_BLACK);
        MULTIEDIT_SetText(hItem, "");
        s_hSmsMsg = hItem;

        /* keyboard: light keys, dark letters; function keys gray, toggles accent */
        {
            int i;
            for (i = 0; i < 26; i++)
            {
                btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_A + i),
                          GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            }
            for (i = 0; i < 10; i++)
            {
                btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_1 + i),
                          GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            }
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_STAR),  GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_SHARP), GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_MINUS), GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_DOT),   GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_COMMA), GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_PLUS),  GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_QM),    GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_EX),    GUI_FONT_24B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_MODE),    &GUI_Font16B_ASCII, UI_COL_ACCENT, GUI_WHITE);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_MODE_N),  &GUI_Font16B_ASCII, UI_COL_ACCENT, GUI_WHITE);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_SPACE_A), &GUI_Font16B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_SPACE_N), &GUI_Font16B_ASCII, UI_COL_KEY, UI_COL_KEYTEXT);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_BKSP_A),  &GUI_Font16B_ASCII, UI_COL_FUNC, GUI_WHITE);
            btn_style(WM_GetDialogItem(pMsg->hWin, ID_KB_BKSP_N),  &GUI_Font16B_ASCII, UI_COL_FUNC, GUI_WHITE);
        }
        s_kb_target_msg = 1;                 /* keyboard writes to MSG by default */
        kb_apply_mode(pMsg->hWin, 0);        /* ABC mode, 123 group hidden */

        btn_style(WM_GetDialogItem(pMsg->hWin, ID_S_DEL),
                  &GUI_Font16B_ASCII, UI_COL_FUNC, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_S_SEND),
                  GUI_FONT_24B_ASCII, UI_COL_OK, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_S_CLEAR),
                  GUI_FONT_24B_ASCII, UI_COL_FUNC, GUI_WHITE);

        hItem = WM_GetDialogItem(pMsg->hWin, ID_S_RESULT);
        TEXT_SetFont(hItem, &GUI_Font16B_ASCII);
        TEXT_SetTextColor(hItem, UI_COL_TEXT);
        TEXT_SetText(hItem, "");
        s_hSmsResult = hItem;
        break;

    case WM_NOTIFY_PARENT:
        Id    = WM_GetId(pMsg->hWinSrc);
        NCode = pMsg->Data.v;

        /* tapping an input field selects where the keyboard goes */
        if (NCode == WM_NOTIFICATION_CLICKED)
        {
            if (Id == ID_S_NUM)
            {
                s_kb_target_msg = 0;
            }
            else if (Id == ID_S_MSG)
            {
                s_kb_target_msg = 1;
            }
        }
        if (NCode != WM_NOTIFICATION_CLICKED)
        {
            break;
        }

        /* keyboard ranges first (buttons are laid out in QWERTY order) */
        if (Id >= ID_KB_A && Id <= ID_KB_A + 25)
        {
            static const char s_kb_letters[27] = "QWERTYUIOPASDFGHJKLZXCVBNM";
            kb_send((int)(s_kb_letters[Id - ID_KB_A] - 'A' + 'a'));
            break;
        }
        if (Id >= ID_KB_1 && Id <= ID_KB_1 + 9)
        {
            static const char s_digits[11] = "1234567890";   /* '1'+9 would be ':' */
            kb_send(s_digits[Id - ID_KB_1]);
            break;
        }

        switch (Id)
        {
        case ID_S_DEL:
            EDIT_AddKey(s_hSmsNum, GUI_KEY_BACKSPACE);
            break;
        case ID_S_CLEAR:
            EDIT_SetText(s_hSmsNum, "");
            MULTIEDIT_SetText(s_hSmsMsg, "");
            TEXT_SetText(s_hSmsResult, "");
            s_result_cache = 0xFF;
            break;
        case ID_S_SEND:
            {
                char num[GSM_NUM_MAX_LEN + 1];
                char msg[GSM_SMS_MAX_LEN + 1];

                EDIT_GetText(s_hSmsNum, num, sizeof(num));
                MULTIEDIT_GetText(s_hSmsMsg, msg, sizeof(msg));

                if (strlen(num) == 0 || strlen(msg) == 0)
                {
                    TEXT_SetText(s_hSmsResult, "EMPTY NUMBER OR MSG");
                    break;
                }
                gsm_req_sms(num, msg);
                MULTIEDIT_SetText(s_hSmsMsg, "");
                TEXT_SetText(s_hSmsResult, "SENDING...");
            }
            break;
        case ID_KB_MODE:
            kb_apply_mode(pMsg->hWin, 1);
            break;
        case ID_KB_MODE_N:
            kb_apply_mode(pMsg->hWin, 0);
            break;
        case ID_KB_STAR:    kb_send('*'); break;
        case ID_KB_SHARP:   kb_send('#'); break;
        case ID_KB_MINUS:   kb_send('-'); break;
        case ID_KB_DOT:     kb_send('.'); break;
        case ID_KB_COMMA:   kb_send(','); break;
        case ID_KB_PLUS:    kb_send('+'); break;
        case ID_KB_QM:      kb_send('?'); break;
        case ID_KB_EX:      kb_send('!'); break;
        case ID_KB_SPACE_A:
        case ID_KB_SPACE_N: kb_send(' '); break;
        case ID_KB_BKSP_A:
        case ID_KB_BKSP_N:  kb_send(GUI_KEY_BACKSPACE); break;
        default:
            break;
        }
        break;

    default:
        WM_DefaultProc(pMsg);
        break;
    }
}

/******************************************************************************************/
/* Inbox page callback */

static void _cbInbox(WM_MESSAGE *pMsg)
{
    WM_HWIN hItem;
    int     Id;
    int     NCode;

    switch (pMsg->MsgId)
    {
    case WM_INIT_DIALOG:
        WINDOW_SetBkColor(pMsg->hWin, UI_COL_BG);

        hItem = WM_GetDialogItem(pMsg->hWin, ID_I_LIST);
        LISTBOX_SetFont(hItem, &GUI_Font16B_ASCII);
        LISTBOX_SetBkColor(hItem, LISTBOX_CI_UNSEL, UI_COL_CARD);
        LISTBOX_SetTextColor(hItem, LISTBOX_CI_UNSEL, GUI_BLACK);
        LISTBOX_SetBkColor(hItem, LISTBOX_CI_SEL, GUI_BLUE);
        LISTBOX_SetTextColor(hItem, LISTBOX_CI_SEL, GUI_WHITE);
        LISTBOX_SetBkColor(hItem, LISTBOX_CI_SELFOCUS, GUI_BLUE);
        LISTBOX_SetTextColor(hItem, LISTBOX_CI_SELFOCUS, GUI_WHITE);
        s_hInboxList = hItem;

        hItem = WM_GetDialogItem(pMsg->hWin, ID_I_MSG);
        TEXT_SetFont(hItem, &GUI_Font16B_ASCII);
        TEXT_SetTextColor(hItem, UI_COL_TEXT);
        s_hInboxMsg = hItem;

        inbox_refresh();
        s_inbox_cache = g_gsm_inbox_dirty;
        break;

    case WM_NOTIFY_PARENT:
        Id    = WM_GetId(pMsg->hWinSrc);
        NCode = pMsg->Data.v;
        if (Id == ID_I_LIST && NCode == WM_NOTIFICATION_SEL_CHANGED)
        {
            inbox_show_sel();
        }
        break;

    default:
        WM_DefaultProc(pMsg);
        break;
    }
}

/******************************************************************************************/
/* AT test page callback */

static void _cbAt(WM_MESSAGE *pMsg)
{
    WM_HWIN hItem;
    int     Id;
    int     NCode;

    switch (pMsg->MsgId)
    {
    case WM_INIT_DIALOG:
        WINDOW_SetBkColor(pMsg->hWin, UI_COL_BG);
        lbl_style(pMsg->hWin, ID_A_LABEL, NULL);

        hItem = WM_GetDialogItem(pMsg->hWin, ID_A_EDIT);
        EDIT_SetFont(hItem, &GUI_Font16B_ASCII);
        EDIT_SetMaxLen(hItem, 60);
        EDIT_SetBkColor(hItem, EDIT_CI_ENABLED, UI_COL_CARD);
        EDIT_SetTextColor(hItem, EDIT_CI_ENABLED, GUI_BLACK);
        s_hAtEdit = hItem;

        btn_style(WM_GetDialogItem(pMsg->hWin, ID_A_SEND),
                  GUI_FONT_24B_ASCII, UI_COL_OK, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_A_AT),
                  GUI_FONT_24B_ASCII, UI_COL_ACCENT, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_A_CSQ),
                  GUI_FONT_24B_ASCII, UI_COL_ACCENT, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_A_REG),
                  GUI_FONT_24B_ASCII, UI_COL_ACCENT, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_A_DEL),
                  &GUI_Font16B_ASCII, UI_COL_FUNC, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_A_CLEAR),
                  &GUI_Font16B_ASCII, UI_COL_FUNC, GUI_WHITE);

        hItem = WM_GetDialogItem(pMsg->hWin, ID_A_SIG);
        TEXT_SetFont(hItem, &GUI_Font16B_ASCII);
        TEXT_SetTextColor(hItem, UI_COL_TEXT);
        TEXT_SetText(hItem, "SIG: N/A");
        s_hAtSig = hItem;

        /* log card: dark background, light terminal text */
        hItem = WM_GetDialogItem(pMsg->hWin, ID_A_LOG);
        TEXT_SetFont(hItem, &GUI_Font13_ASCII);
        TEXT_SetBkColor(hItem, GUI_DARKGRAY);
        TEXT_SetTextColor(hItem, GUI_WHITE);
        TEXT_SetText(hItem, "");
        s_hAtLog = hItem;
        break;

    case WM_NOTIFY_PARENT:
        Id    = WM_GetId(pMsg->hWinSrc);
        NCode = pMsg->Data.v;
        if (NCode != WM_NOTIFICATION_CLICKED)
        {
            break;
        }
        switch (Id)
        {
        case ID_A_DEL:
            EDIT_AddKey(s_hAtEdit, GUI_KEY_BACKSPACE);
            break;
        case ID_A_SEND:
            {
                char cmd[64];
                EDIT_GetText(s_hAtEdit, cmd, sizeof(cmd));
                if (strlen(cmd) > 0)
                {
                    gsm_req_at(cmd);
                }
            }
            break;
        case ID_A_CSQ:
            gsm_req_at("AT+CSQ");
            break;
        case ID_A_AT:
            gsm_req_at("AT");
            break;
        case ID_A_REG:
            gsm_req_at("AT+CREG?");
            break;
        case ID_A_CLEAR:
            gsm_at_clear_log();
            TEXT_SetText(s_hAtLog, "");
            s_atlog_cache = g_gsm_at_log_dirty;
            break;
        default:
            break;
        }
        break;

    default:
        WM_DefaultProc(pMsg);
        break;
    }
}

/******************************************************************************************/
/* Incoming-call popup callback */

static void _cbCallPopup(WM_MESSAGE *pMsg)
{
    WM_HWIN hItem;
    int     Id;
    int     NCode;

    switch (pMsg->MsgId)
    {
    case WM_INIT_DIALOG:
        WINDOW_SetBkColor(pMsg->hWin, GUI_DARKGRAY);

        hItem = WM_GetDialogItem(pMsg->hWin, ID_P_TITLE);
        TEXT_SetFont(hItem, GUI_FONT_24B_ASCII);
        TEXT_SetTextColor(hItem, GUI_YELLOW);
        TEXT_SetTextAlign(hItem, GUI_TA_HCENTER | GUI_TA_VCENTER);
        TEXT_SetText(hItem, "INCOMING CALL");

        hItem = WM_GetDialogItem(pMsg->hWin, ID_P_NUM);
        TEXT_SetFont(hItem, GUI_FONT_24B_ASCII);
        TEXT_SetTextColor(hItem, GUI_WHITE);
        TEXT_SetTextAlign(hItem, GUI_TA_HCENTER | GUI_TA_VCENTER);
        TEXT_SetText(hItem, "UNKNOWN");
        s_hCallPopupNum = hItem;

        btn_style(WM_GetDialogItem(pMsg->hWin, ID_P_ANS),
                  GUI_FONT_24B_ASCII, UI_COL_OK, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_P_REJ),
                  GUI_FONT_24B_ASCII, UI_COL_NO, GUI_WHITE);
        break;

    case WM_NOTIFY_PARENT:
        Id    = WM_GetId(pMsg->hWinSrc);
        NCode = pMsg->Data.v;
        if (NCode != WM_NOTIFICATION_CLICKED)
        {
            break;
        }
        if (Id == ID_P_ANS)
        {
            gsm_req_answer();
        }
        else if (Id == ID_P_REJ)
        {
            gsm_req_hangup();
        }
        break;

    default:
        WM_DefaultProc(pMsg);
        break;
    }
}

/******************************************************************************************/
/* New-SMS popup callback */

static void _cbSmsPopup(WM_MESSAGE *pMsg)
{
    WM_HWIN hItem;
    int     Id;
    int     NCode;

    switch (pMsg->MsgId)
    {
    case WM_INIT_DIALOG:
        WINDOW_SetBkColor(pMsg->hWin, GUI_DARKGRAY);

        hItem = WM_GetDialogItem(pMsg->hWin, ID_N_TITLE);
        TEXT_SetFont(hItem, GUI_FONT_24B_ASCII);
        TEXT_SetTextColor(hItem, GUI_YELLOW);
        TEXT_SetTextAlign(hItem, GUI_TA_HCENTER | GUI_TA_VCENTER);
        TEXT_SetText(hItem, "NEW SMS");

        hItem = WM_GetDialogItem(pMsg->hWin, ID_N_FROM);
        TEXT_SetFont(hItem, &GUI_Font16B_ASCII);
        TEXT_SetTextColor(hItem, GUI_CYAN);
        TEXT_SetTextAlign(hItem, GUI_TA_HCENTER | GUI_TA_VCENTER);
        s_hSmsPopupFrom = hItem;

        hItem = WM_GetDialogItem(pMsg->hWin, ID_N_MSG);
        TEXT_SetFont(hItem, &GUI_Font16B_ASCII);
        TEXT_SetTextColor(hItem, GUI_WHITE);
        TEXT_SetTextAlign(hItem, GUI_TA_HCENTER | GUI_TA_VCENTER);
        s_hSmsPopupMsg = hItem;

        btn_style(WM_GetDialogItem(pMsg->hWin, ID_N_VIEW),
                  GUI_FONT_24B_ASCII, UI_COL_OK, GUI_WHITE);
        btn_style(WM_GetDialogItem(pMsg->hWin, ID_N_CLOSE),
                  GUI_FONT_24B_ASCII, UI_COL_FUNC, GUI_WHITE);
        break;

    case WM_NOTIFY_PARENT:
        Id    = WM_GetId(pMsg->hWinSrc);
        NCode = pMsg->Data.v;
        if (NCode != WM_NOTIFICATION_CLICKED)
        {
            break;
        }
        if (Id == ID_N_VIEW)
        {
            MULTIPAGE_SelectPage(s_hMp, 2);         /* jump to the INBOX page */
        }
        if (s_hSmsPopup != 0)
        {
            WM_HideWindow(s_hSmsPopup);
        }
        s_inbox_seen = g_gsm_inbox_cnt;
        break;

    default:
        WM_DefaultProc(pMsg);
        break;
    }
}

/******************************************************************************************/
/* Main dialog callback */

static void _cbMain(WM_MESSAGE *pMsg)
{
    WM_HWIN hItem;
    WM_HWIN hPage;
    int     Id;
    int     NCode;

    switch (pMsg->MsgId)
    {
    case WM_INIT_DIALOG:
        hItem = pMsg->hWin;                     /* framewin, hide the title bar */
        FRAMEWIN_SetTitleVis(hItem, 0);
        FRAMEWIN_SetClientColor(hItem, UI_COL_BG);

        s_hMp = WM_GetDialogItem(pMsg->hWin, ID_MP);
        MULTIPAGE_SetFont(s_hMp, &GUI_Font20B_ASCII);
        MULTIPAGE_SetBkColor(s_hMp, UI_COL_BG, MULTIPAGE_CI_ENABLED);
        MULTIPAGE_SetTextColor(s_hMp, GUI_WHITE,    MULTIPAGE_BI_SELECTED);
        MULTIPAGE_SetTextColor(s_hMp, UI_COL_SUBTEXT, MULTIPAGE_BI_UNSELECTED);
        MULTIPAGE_SetTextColor(s_hMp, UI_COL_SUBTEXT, MULTIPAGE_BI_DISABLED);

        /* build the four pages */
        hPage = GUI_CreateDialogBox(_aDialCreate, GUI_COUNTOF(_aDialCreate), _cbDial, s_hMp, 0, 0);
        MULTIPAGE_AddPage(s_hMp, hPage, "DIAL");
        hPage = GUI_CreateDialogBox(_aSmsCreate, GUI_COUNTOF(_aSmsCreate), _cbSms, s_hMp, 0, 0);
        MULTIPAGE_AddPage(s_hMp, hPage, "SMS");
        hPage = GUI_CreateDialogBox(_aInboxCreate, GUI_COUNTOF(_aInboxCreate), _cbInbox, s_hMp, 0, 0);
        MULTIPAGE_AddPage(s_hMp, hPage, "INBOX");
        hPage = GUI_CreateDialogBox(_aAtCreate, GUI_COUNTOF(_aAtCreate), _cbAt, s_hMp, 0, 0);
        MULTIPAGE_AddPage(s_hMp, hPage, "AT");
        MULTIPAGE_SelectPage(s_hMp, 0);

        s_hStat = WM_GetDialogItem(pMsg->hWin, ID_TEXT_STAT);
        TEXT_SetFont(s_hStat, &GUI_Font16B_ASCII);
        TEXT_SetBkColor(s_hStat, UI_COL_STATBAR);
        TEXT_SetTextColor(s_hStat, GUI_WHITE);
        TEXT_SetText(s_hStat, "GSM: INIT...");

        s_hHint = WM_GetDialogItem(pMsg->hWin, ID_TEXT_HINT);
        TEXT_SetFont(s_hHint, &GUI_Font13_ASCII);
        TEXT_SetBkColor(s_hHint, UI_COL_BG);
        TEXT_SetTextColor(s_hHint, UI_COL_SUBTEXT);
        TEXT_SetText(s_hHint, "CMD:LED0/LED1/BEEP/ALL ON|OFF,STATUS");
        break;

    case WM_NOTIFY_PARENT:
        Id    = WM_GetId(pMsg->hWinSrc);
        NCode = pMsg->Data.v;
        if (Id == ID_MP && NCode == WM_NOTIFICATION_VALUE_CHANGED)
        {
            /* force an inbox rebuild when the user switches to it */
            if (MULTIPAGE_GetSelection(s_hMp) == 2)
            {
                s_inbox_cache = 0xFFFFFFFF;
            }
        }
        break;

    default:
        WM_DefaultProc(pMsg);
        break;
    }
}

/******************************************************************************************/
/* Public API */

/**
 * @brief       Create the whole UI on the desktop window. Call once after GUI_Init().
 */
void gsm_ui_create(void)
{
    ui_style_init();                            /* global look, before any widget exists */
    GUI_CreateDialogBox(_aMainCreate, GUI_COUNTOF(_aMainCreate), _cbMain, WM_HBKWIN, 0, 0);
}

/**
 * @brief       Refresh dynamic widgets. Call periodically from the UI-owning task.
 */
void gsm_ui_update(void)
{
    char     stat[80];
    char     call[120];
    char     state_txt[16];
    char     num_txt[GSM_NUM_MAX_LEN + 1];
    uint8_t  result;
    uint8_t  ready;
    uint8_t  sig;
    uint8_t  call_state;

    if (s_hStat == 0)
    {
        return;                                 /* not created yet */
    }

    ready      = g_gsm_ready;
    sig        = g_gsm_signal;
    call_state = g_gsm_call_state;
    gsm_format_call(state_txt, num_txt);

    /* ---- bottom status line: call state has priority, then module + signal ---- */
    if (!ready)
    {
        strcpy(stat, "GSM: NO MODULE / CHECK SIM");
    }
    else if (call_state != GSM_CALL_IDLE)
    {
        if (num_txt[0] != 0)
        {
            snprintf(stat, sizeof(stat), "CALL:%s %s", state_txt, num_txt);
        }
        else
        {
            snprintf(stat, sizeof(stat), "CALL:%s", state_txt);
        }
    }
    else
    {
        char csqtxt[28];

        if (sig == 99)
        {
            strcpy(csqtxt, "CSQ:N/A");
        }
        else if (sig >= 2 && sig <= 31)
        {
            snprintf(csqtxt, sizeof(csqtxt), "CSQ:%d(%ddBm)", (int)sig, -113 + 2 * (int)sig);
        }
        else
        {
            snprintf(csqtxt, sizeof(csqtxt), "CSQ:%d", (int)sig);
        }

        if (g_gsm_reg == GSM_REG_OK)
        {
            snprintf(stat, sizeof(stat), "GSM:READY %s REG:OK", csqtxt);
        }
        else if (g_gsm_reg == GSM_REG_OFFLINE)
        {
            snprintf(stat, sizeof(stat), "GSM:READY %s REG:OFFLINE", csqtxt);
        }
        else if (g_gsm_reg == GSM_REG_DENIED)
        {
            snprintf(stat, sizeof(stat), "GSM:READY %s REG:DENIED", csqtxt);
        }
        else if (g_gsm_reg == GSM_REG_SEARCHING)
        {
            snprintf(stat, sizeof(stat), "GSM:READY %s REG:SEARCH", csqtxt);
        }
        else
        {
            snprintf(stat, sizeof(stat), "GSM:READY %s", csqtxt);
        }
    }
    if (strcmp(stat, s_stat_cache) != 0)
    {
        strcpy(s_stat_cache, stat);
        TEXT_SetText(s_hStat, stat);
    }

    /* ---- dial page: call state / last failure ---- */
    if (call_state == GSM_CALL_IDLE && g_gsm_call_fail != GSM_DIAL_FAIL_NONE)
    {
        if (g_gsm_call_fail == GSM_DIAL_FAIL_ERROR)
        {
            strcpy(call, "CALL: FAIL (MODULE ERROR)");
        }
        else
        {
            strcpy(call, "CALL: FAIL (NO ANSWER)");
        }
    }
    else if (num_txt[0] != 0)
    {
        snprintf(call, sizeof(call), "CALL: %s %s", state_txt, num_txt);
    }
    else
    {
        snprintf(call, sizeof(call), "CALL: %s", state_txt);
    }
    if (strcmp(call, s_call_cache) != 0)
    {
        strcpy(s_call_cache, call);
        TEXT_SetText(s_hCallInfo, call);
    }

    /* ---- AT page: signal + registration line ---- */
    {
        char sigtxt[64];
        char sigpart[24];
        const char *regtxt;

        if (sig == 99)
        {
            strcpy(sigpart, "N/A");
        }
        else if (sig >= 2 && sig <= 31)
        {
            snprintf(sigpart, sizeof(sigpart), "%d (%d dBm)", (int)sig, -113 + 2 * (int)sig);
        }
        else
        {
            snprintf(sigpart, sizeof(sigpart), "%d", (int)sig);
        }

        switch (g_gsm_reg)
        {
        case GSM_REG_OK:        regtxt = "OK";        break;
        case GSM_REG_OFFLINE:   regtxt = "OFFLINE";   break;
        case GSM_REG_DENIED:    regtxt = "DENIED";    break;
        case GSM_REG_SEARCHING: regtxt = "SEARCHING"; break;
        default:                regtxt = "?";         break;
        }

        snprintf(sigtxt, sizeof(sigtxt), "SIG: %s  REG: %s", sigpart, regtxt);
        if (strcmp(sigtxt, s_sig_cache) != 0)
        {
            strcpy(s_sig_cache, sigtxt);
            TEXT_SetText(s_hAtSig, sigtxt);
        }
    }

    /* ---- AT page: rolling response log ---- */
    if (g_gsm_at_log_dirty != s_atlog_cache)
    {
        s_atlog_cache = g_gsm_at_log_dirty;
        gsm_at_get_log(s_logbuf, sizeof(s_logbuf));
        TEXT_SetText(s_hAtLog, s_logbuf);
    }

    /* ---- sms page: send result ---- */
    result = gsm_get_sms_result();
    if (result != s_result_cache)
    {
        s_result_cache = result;
        switch (result)
        {
        case GSM_SMS_ST_SENDING: TEXT_SetText(s_hSmsResult, "SENDING...");   break;
        case GSM_SMS_ST_OK:      TEXT_SetText(s_hSmsResult, "SEND OK");      break;
        case GSM_SMS_ST_FAIL:    TEXT_SetText(s_hSmsResult, "SEND FAIL");    break;
        default:                 TEXT_SetText(s_hSmsResult, "");             break;
        }
    }

    /* ---- inbox page: rebuild on change ---- */
    if (g_gsm_inbox_dirty != s_inbox_cache)
    {
        s_inbox_cache = g_gsm_inbox_dirty;
        inbox_refresh();
    }

    /* ---- incoming-call popup (any page) ---- */
    if (call_state == GSM_CALL_INCOMING)
    {
        if (s_hCallPopup == 0)
        {
            s_hCallPopup = GUI_CreateDialogBox(_aCallPopupCreate,
                                               GUI_COUNTOF(_aCallPopupCreate),
                                               _cbCallPopup, WM_HBKWIN, 0, 0);
            WM_HideWindow(s_hCallPopup);
        }
        if (strcmp(num_txt, s_popup_num_cache) != 0)
        {
            strcpy(s_popup_num_cache, num_txt);
            TEXT_SetText(s_hCallPopupNum, num_txt[0] ? num_txt : "UNKNOWN");
        }
        if (!WM_IsVisible(s_hCallPopup))
        {
            WM_ShowWindow(s_hCallPopup);
        }
    }
    else if (s_hCallPopup != 0 && WM_IsVisible(s_hCallPopup))
    {
        WM_HideWindow(s_hCallPopup);
    }

    /* ---- new-SMS popup (suppressed during a call or on the INBOX page) ---- */
    if (g_gsm_inbox_cnt != s_inbox_seen)
    {
        uint8_t is_new = (g_gsm_inbox_cnt > s_inbox_seen) ? 1 : 0;

        s_inbox_seen = g_gsm_inbox_cnt;
        if (is_new && call_state != GSM_CALL_INCOMING &&
            s_hMp != 0 && MULTIPAGE_GetSelection(s_hMp) != 2)
        {
            char pnum[GSM_NUM_MAX_LEN + 1];
            char pmsg[GSM_SMS_MAX_LEN + 1];
            char from[48];
            char body[100];

            CPU_SR_ALLOC();
            CPU_CRITICAL_ENTER();
            {
                int slot = (int)((g_gsm_inbox_cnt - 1) % GSM_INBOX_MAX);
                strncpy(pnum, g_gsm_inbox[slot].num, GSM_NUM_MAX_LEN);
                pnum[GSM_NUM_MAX_LEN] = 0;
                strncpy(pmsg, g_gsm_inbox[slot].msg, GSM_SMS_MAX_LEN);
                pmsg[GSM_SMS_MAX_LEN] = 0;
            }
            CPU_CRITICAL_EXIT();

            snprintf(from, sizeof(from), "FROM: %s", pnum);
            wrap_text(body, sizeof(body), pmsg, 30);

            if (s_hSmsPopup == 0)
            {
                s_hSmsPopup = GUI_CreateDialogBox(_aSmsPopupCreate,
                                                  GUI_COUNTOF(_aSmsPopupCreate),
                                                  _cbSmsPopup, WM_HBKWIN, 0, 0);
                WM_HideWindow(s_hSmsPopup);
            }
            TEXT_SetText(s_hSmsPopupFrom, from);
            TEXT_SetText(s_hSmsPopupMsg, body);
            WM_ShowWindow(s_hSmsPopup);
        }
    }
}

/******************************************************************************************/
