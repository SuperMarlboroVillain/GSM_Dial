/**
 ****************************************************************************************************
 * @file        gsm.c
 * @brief       GSM module AT command engine (SIM800C/SIM900 series) on USART1.
 *
 * Architecture notes:
 *  - RX path reuses the ring-buffer line receiver in SYSTEM/usart (usart_get_line /
 *    usart_rx_pop / usart_rx_flush): the USART1 ISR pushes every byte into a ring,
 *    lines are extracted on the task side, so back-to-back response lines never
 *    lose bytes. This task is the only consumer.
 *  - This file owns a small dispatcher: any complete line that is not consumed by a
 *    blocking AT command wait is treated as an URC (RING / +CLIP / +CMTI / call end
 *    reports ...) and handled in gsm_poll().
 *  - The task is cooperative: commands wait with timeouts, URCs are drained between
 *    and inside command waits, so nothing is lost while a command is in flight.
 *  - SMS control commands (LED0/LED1/BEEP/ALL/STATUS, ON/OFF, case-insensitive) are
 *    executed in cmd_exec() and acknowledged with a reply SMS to the sender.
 ****************************************************************************************************
 */

#include "gsm.h"
#include "./SYSTEM/usart/usart.h"
#include "./BSP/LED/led.h"
#include "./BSP/BEEP/beep.h"
#include "os.h"
#include "cpu.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/******************************************************************************************/
/* Shared state definitions */

volatile uint8_t  g_gsm_ready      = 0;
volatile uint8_t  g_gsm_signal     = 99;
volatile uint8_t  g_gsm_call_state = GSM_CALL_IDLE;
volatile uint8_t  g_gsm_call_evt   = 0;
volatile uint8_t  g_gsm_call_fail  = GSM_DIAL_FAIL_NONE;
volatile uint8_t  g_gsm_reg        = GSM_REG_UNKNOWN;
volatile char     g_gsm_call_num[GSM_NUM_MAX_LEN + 1] = {0};
volatile uint8_t  g_gsm_sms_result = GSM_SMS_ST_NONE;
volatile uint32_t g_gsm_inbox_cnt  = 0;
volatile uint32_t g_gsm_inbox_dirty = 0;
volatile uint32_t g_gsm_at_log_dirty = 0;
gsm_sms_t         g_gsm_inbox[GSM_INBOX_MAX];

/******************************************************************************************/
/* Private state */

#define GSM_LINE_MAX    200

static char     s_line[GSM_LINE_MAX];           /* scratch line buffer (this task only) */
static char     s_last_line[GSM_LINE_MAX];      /* last non-URC line seen, for parsing  */

/* request slots (UI -> gsm task) */
static volatile uint8_t s_req_dial  = 0;
static volatile uint8_t s_req_hang  = 0;
static volatile uint8_t s_req_ans   = 0;
static volatile uint8_t s_req_sms   = 0;
static volatile char    s_req_dial_num[GSM_NUM_MAX_LEN + 1];
static volatile char    s_req_sms_num[GSM_NUM_MAX_LEN + 1];
static volatile char    s_req_msg[GSM_SMS_MAX_LEN + 1];

/* pending incoming SMS index reported by +CMTI */
static volatile int16_t s_pending_sms = -1;
static volatile char    s_pending_mem[8] = {0}; /* storage from +CMTI ("SM"/"ME") */

/* auto reply queue (single slot, command answers) */
static volatile uint8_t s_reply_req = 0;
static volatile char    s_reply_num[GSM_NUM_MAX_LEN + 1];
static volatile char    s_reply_msg[GSM_SMS_MAX_LEN + 1];

/* AT test console: rolling response log + request slot */
#define GSM_AT_LOG_MAX      2048
static volatile char     s_at_log[GSM_AT_LOG_MAX];
static volatile uint16_t s_at_log_len = 0;
static volatile uint8_t  s_req_at = 0;
static volatile char     s_req_at_cmd[80];

/* misc timers */
static volatile uint32_t s_init_retry_tmr = 0;
static volatile uint32_t s_csq_tmr        = 0;
static volatile uint16_t s_cnmi_retry_tmr = 0;
static volatile uint16_t s_cmgl_tmr       = 3000;  /* first SMS sweep 30 s after ready */

/* background housekeeping (periodic CSQ/CREG/CNMI/sweep/init retries) never
 * appears in the AT console: the console only shows user-pressed commands,
 * their responses and URC/event lines */
static volatile uint8_t  s_log_silent     = 0;
/* CNMI is essential for push delivery; retry until the module accepts it */
static volatile uint8_t  s_cnmi_ok        = 0;

/* NOTE: never printf() in this project - fputc() writes to USART1, which is
 * the GSM module link; injected bytes would corrupt the AT command stream. */

/* forward declarations */
static void gsm_dispatch_urc(const char *l);
static int  gsm_cmd(const char *cmd, const char *want, uint32_t timeout);
static int  gsm_sms_send(const char *num, const char *msg);
static void gsm_sms_read(int idx);
static void gsm_inbox_add(const char *num, const char *msg);
static void gsm_reply_queue(const char *num, const char *msg);
static void gsm_cmd_exec(const char *num, const char *msg);
static void gsm_poll(void);
static void gsm_log_append(const char *txt);
static void gsm_serve_reply(void);
static void gsm_sms_sweep(void);
static int  gsm_wait_prompt(uint32_t timeout);

/******************************************************************************************/
/* Low level helpers */

/**
 * @brief       Send raw bytes to the module.
 * @param       data : buffer
 * @param       len  : length
 * @retval      none
 */
static void gsm_write(const uint8_t *data, uint16_t len)
{
    HAL_UART_Transmit(&g_uart1_handle, (uint8_t *)data, len, 1000);
}

/**
 * @brief       Send an ASCII string to the module.
 */
static void gsm_puts(const char *s)
{
    gsm_write((const uint8_t *)s, (uint16_t)strlen(s));
}

/**
 * @brief       Delay helper (1 tick granularity, 1000 Hz tick).
 */
static void gsm_delay_ms(uint32_t ms)
{
    OS_ERR err;
    while (ms-- > 0)
    {
        OSTimeDly(1, OS_OPT_TIME_DLY, &err);
    }
}

/**
 * @brief       Fetch one complete line (without CRLF) from the usart line receiver.
 * @param       out : destination buffer
 * @param       max : buffer size
 * @retval      1: a line was consumed (may be empty), 0: nothing complete yet
 */
static int gsm_get_line(char *out, int max)
{
    return usart_get_line(out, max);
}

/**
 * @brief       Flush the usart receiver (drop pending garbage bytes).
 */
static void gsm_rx_flush(void)
{
    usart_rx_flush();
}

/**
 * @brief       Append one line to the rolling AT console log (keeps the newest
 *              half when the buffer runs full).
 */
static void gsm_log_append(const char *txt)
{
    CPU_SR_ALLOC();
    uint16_t n = (uint16_t)strlen(txt);

    CPU_CRITICAL_ENTER();
    if ((uint16_t)(s_at_log_len + n + 1) >= (uint16_t)GSM_AT_LOG_MAX)
    {
        uint16_t drop = GSM_AT_LOG_MAX / 2;
        memmove((void *)s_at_log, (const void *)(s_at_log + drop),
                (uint16_t)(s_at_log_len - drop));
        s_at_log_len = (uint16_t)(s_at_log_len - drop);
    }
    memcpy((void *)(s_at_log + s_at_log_len), txt, n);
    s_at_log_len = (uint16_t)(s_at_log_len + n);
    s_at_log[s_at_log_len++] = '\n';
    s_at_log[s_at_log_len] = 0;
    g_gsm_at_log_dirty++;
    CPU_CRITICAL_EXIT();
}

/**
 * @brief       Check whether a received line is an unsolicited result code (URC)
 *              that the dispatcher must handle instead of the command waiter.
 */
static int gsm_is_urc(const char *l)
{
    if (!strcmp(l, "RING"))            return 1;
    if (!strncmp(l, "+CLIP:", 6))      return 1;
    if (!strncmp(l, "+CMTI:", 6))      return 1;
    if (!strncmp(l, "+CMT:", 5))       return 1;
    if (!strncmp(l, "+CREG:", 6))      return 1;
    if (!strncmp(l, "+CGREG:", 7))     return 1;
    if (!strncmp(l, "+CEREG:", 7))     return 1;
    if (!strncmp(l, "+CSQ:", 5))       return 1;
    if (!strncmp(l, "+CPIN:", 6))      return 1;
    if (!strncmp(l, "+CGEV:", 6))      return 1;
    if (!strcmp(l, "NO CARRIER"))      return 1;
    if (!strcmp(l, "BUSY"))            return 1;
    if (!strcmp(l, "NO ANSWER"))       return 1;
    if (!strcmp(l, "NO DIALTONE"))     return 1;
    if (!strcmp(l, "RDY"))             return 1;   /* SIM7600 boot URCs */
    if (!strcmp(l, "SMS DONE"))        return 1;
    if (!strcmp(l, "PB DONE"))         return 1;
    if (!strcmp(l, "SMS READY"))       return 1;
    if (!strcmp(l, "PB READY"))        return 1;
    return 0;
}

/**
 * @brief       Extract the first quoted string of a line ("+CLIP: \"138..\",129.." -> 138..).
 * @param       l   : line
 * @param       out : destination
 * @param       max : destination size
 * @retval      1 on success
 */
static int gsm_pick_quoted(const char *l, char *out, int max)
{
    const char *p = strchr(l, '"');
    int i = 0;

    if (p == NULL)
    {
        out[0] = 0;
        return 0;
    }
    p++;
    while (*p && *p != '"' && i < max - 1)
    {
        out[i++] = *p++;
    }
    out[i] = 0;
    return (i > 0);
}

/**
 * @brief       Extract the sender number from a text-mode +CMGR/+CMGL header line.
 *              The FIRST quoted field of those responses is the message status
 *              ("REC UNREAD" etc.), the number is the first quoted field that
 *              actually looks like a number (starts with '+' or a digit).
 * @param       l   : header line
 * @param       out : destination
 * @param       max : destination size
 * @retval      1 on success
 */
static int gsm_pick_sender(const char *l, char *out, int max)
{
    const char *p = l;

    out[0] = 0;
    while ((p = strchr(p, '"')) != NULL)
    {
        char tok[GSM_NUM_MAX_LEN + 1];
        int i = 0;

        p++;
        while (*p && *p != '"' && i < GSM_NUM_MAX_LEN)
        {
            tok[i++] = *p++;
        }
        tok[i] = 0;
        if (*p == '"')
        {
            p++;                                /* skip the closing quote */
        }
        if (tok[0] == '+' || isdigit((unsigned char)tok[0]))
        {
            strncpy(out, tok, (size_t)max - 1);
            out[max - 1] = 0;
            return 1;
        }
    }
    return 0;
}

/**
 * @brief       Reduce a received SMS text to clean printable ASCII, in place:
 *              drop control/high bytes, collapse repeated spaces and trim
 *              leading/trailing spaces. Phones frequently send UCS-2
 *              ("L\0E\0D\0...") or UTF-8; those interleaved NUL/high bytes
 *              garble the inbox display and break the remote-control parser.
 * @param       s : text, cleaned in place
 */
static void gsm_ascii_clean(char *s)
{
    int rd = 0;
    int wr = 0;

    while (s[rd] != 0)
    {
        unsigned char c = (unsigned char)s[rd++];

        if (c < 0x20 || c > 0x7E)
        {
            continue;                           /* drop control / high bytes */
        }
        if (c == ' ' && (wr == 0 || s[wr - 1] == ' '))
        {
            continue;                           /* collapse spaces, skip leading */
        }
        s[wr++] = (char)c;
    }
    while (wr > 0 && s[wr - 1] == ' ')
    {
        wr--;                                   /* trim trailing spaces */
    }
    s[wr] = 0;
}

/******************************************************************************************/
/* URC / event handling */
/**
 * @brief       Map an AT+CREG stat value to g_gsm_reg.
 */
static void gsm_reg_update(int stat)
{
    switch (stat)
    {
    case 1:
    case 5:  g_gsm_reg = GSM_REG_OK;        break;
    case 3:  g_gsm_reg = GSM_REG_DENIED;    break;
    case 0:  g_gsm_reg = GSM_REG_OFFLINE;   break;
    default: g_gsm_reg = GSM_REG_SEARCHING; break;
    }
}

/**
 * @brief       Dispatch one URC line, update call state / pending SMS.
 */
static void gsm_dispatch_urc(const char *l)
{
    gsm_log_append(l);                  /* keep URCs visible in the AT console */

    if (!strcmp(l, "RING"))
    {
        if (g_gsm_call_state == GSM_CALL_IDLE)
        {
            /* alert beep on the first ring, only if the buzzer is currently off */
            if (HAL_GPIO_ReadPin(BEEP_GPIO_PORT, BEEP_GPIO_PIN) == GPIO_PIN_RESET)
            {
                BEEP(1);
                gsm_delay_ms(120);
                BEEP(0);
            }
            g_gsm_call_state = GSM_CALL_INCOMING;
            g_gsm_call_fail  = GSM_DIAL_FAIL_NONE;
            g_gsm_call_evt++;
        }
        else if (g_gsm_call_state == GSM_CALL_INCOMING)
        {
            g_gsm_call_evt++;           /* repeated rings refresh the UI */
        }
    }
    else if (!strncmp(l, "+CLIP:", 6))
    {
        if (g_gsm_call_state == GSM_CALL_INCOMING)
        {
            gsm_pick_quoted(l, (char *)g_gsm_call_num, GSM_NUM_MAX_LEN + 1);
            g_gsm_call_evt++;
        }
    }
    else if (!strncmp(l, "+CMTI:", 6))
    {
        /* +CMTI: "SM",3  /  +CMTI: "ME",0  - keep storage and index */
        const char *p = strchr(l, '"');
        const char *c;

        if (p != NULL)
        {
            int i = 0;
            p++;
            while (*p && *p != '"' && i < (int)sizeof(s_pending_mem) - 1)
            {
                s_pending_mem[i++] = *p++;
            }
            s_pending_mem[i] = 0;
        }
        c = strchr(l, ',');
        if (c != NULL)
        {
            int idx = atoi(c + 1);
            if (idx >= 0 && idx < 250)
            {
                s_pending_sms = (int16_t)idx;
            }
        }
    }
    else if (!strncmp(l, "+CMT:", 5))        /* direct-push mode, not used with CNMI 2,1 */
    {
        /* ignored, CNMI is configured for +CMTI storage indications */
    }
    else if (!strncmp(l, "+CSQ:", 5))
    {
        int rssi = atoi(l + 5);
        if (rssi >= 0 && rssi <= 31)
        {
            g_gsm_signal = (uint8_t)rssi;
        }
        else
        {
            g_gsm_signal = 99;
        }
    }
    else if (!strncmp(l, "+CREG:", 6) || !strncmp(l, "+CGREG:", 7) ||
             !strncmp(l, "+CEREG:", 7))
    {
        /* registration URC: "+CREG: <n>,<stat>" or "<stat>" */
        const char *p = strchr(l, ',');
        gsm_reg_update((p != NULL) ? atoi(p + 1) : atoi(strchr(l, ':') + 1));
    }
    else if (!strncmp(l, "+CPIN:", 6) || !strncmp(l, "+CGEV:", 6) ||
             !strcmp(l, "RDY")     || !strcmp(l, "SMS DONE") ||
             !strcmp(l, "PB DONE") || !strcmp(l, "SMS READY") ||
             !strcmp(l, "PB READY"))
    {
        /* SIM7600 boot / status URCs: nothing to do */
    }
    else if (!strcmp(l, "NO CARRIER") || !strcmp(l, "BUSY") ||
             !strcmp(l, "NO ANSWER") || !strcmp(l, "NO DIALTONE"))
    {
        g_gsm_call_state = GSM_CALL_IDLE;
        g_gsm_call_num[0] = 0;
        g_gsm_call_evt++;
    }
}

/**
 * @brief       Drain all complete lines; URCs are dispatched, others are stored into
 *              s_last_line. Non-blocking.
 */
static void gsm_drain_lines(void)
{
    while (gsm_get_line(s_line, GSM_LINE_MAX))
    {
        if (s_line[0] == 0)
        {
            continue;                       /* ignore empty lines */
        }
        if (gsm_is_urc(s_line))
        {
            gsm_dispatch_urc(s_line);
        }
        else
        {
            strcpy(s_last_line, s_line);
        }
    }
}

/******************************************************************************************/
/* AT command helpers (blocking, only called from this task) */

/**
 * @brief       Send an AT command and wait for an expected token in any response line.
 *              URCs arriving while waiting are dispatched, not lost.
 * @param       cmd     : command string (including CRLF)
 * @param       want    : token to wait for, NULL = wait for any non-URC line
 * @param       timeout : wait timeout in ms
 * @retval      1: token seen, -1: ERROR seen, 0: timeout
 */
static int gsm_cmd(const char *cmd, const char *want, uint32_t timeout)
{
    if (cmd != NULL)
    {
        char logbuf[36];
        uint16_t n;

        gsm_puts(cmd);

        /* mirror the command into the AT console log (without trailing CRLF) */
        if (!s_log_silent)
        {
            n = (uint16_t)strlen(cmd);
            while (n > 0 && (cmd[n - 1] == '\r' || cmd[n - 1] == '\n'))
            {
                n--;
            }
            if (n > (uint16_t)(sizeof(logbuf) - 3))
            {
                n = (uint16_t)(sizeof(logbuf) - 3);
            }
            logbuf[0] = '>';
            logbuf[1] = ' ';
            memcpy(logbuf + 2, cmd, n);
            logbuf[2 + n] = 0;
            gsm_log_append(logbuf);
        }
    }

    while (timeout-- > 0)
    {
        if (gsm_get_line(s_line, GSM_LINE_MAX))
        {
            if (s_line[0] == 0)
            {
                continue;
            }
            if (gsm_is_urc(s_line))
            {
                gsm_dispatch_urc(s_line);
                continue;
            }
            if (!s_log_silent)
            {
                gsm_log_append(s_line);     /* response lines go to the AT console */
            }
            if (strstr(s_line, "ERROR") != NULL)
            {
                return -1;
            }
            if (want == NULL || strstr(s_line, want) != NULL)
            {
                return 1;               /* matched line is not stored in s_last_line */
            }
            strcpy(s_last_line, s_line);
        }
        gsm_delay_ms(1);
    }
    return 0;
}

/******************************************************************************************/
/* SMS */

/**
 * @brief       Wait for the module's '>' SMS prompt (it has no CRLF, so the
 *              normal line path never sees it). URCs keep being dispatched
 *              while waiting; any other pending bytes are consumed.
 * @param       timeout : wait timeout in ms
 * @retval      1: prompt seen, 0: timeout
 */
static int gsm_wait_prompt(uint32_t timeout)
{
    uint8_t b;

    while (timeout-- > 0)
    {
        gsm_drain_lines();                  /* complete lines (URCs) keep flowing */
        while (usart_rx_pop(&b))
        {
            if (b == '>')
            {
                return 1;
            }
        }
        gsm_delay_ms(1);
    }
    return 0;
}

/**
 * @brief       Send one SMS in text mode.
 * @param       num : destination number (digits, may start with '+')
 * @param       msg : ASCII text
 * @retval      1 on success
 */
static int gsm_sms_send(const char *num, const char *msg)
{
    char  cmd[48];
    int   ret = 0;
    uint32_t left;

    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", num);
    gsm_puts(cmd);

    if (!gsm_wait_prompt(5000))             /* wait for the '>' prompt (has no CRLF) */
    {
        return 0;
    }

    gsm_puts(msg);
    gsm_write((const uint8_t *)"\x1A", 1);
    gsm_puts("\r\n");

    left = 8000;
    while (left-- > 0)
    {
        if (gsm_get_line(s_line, GSM_LINE_MAX))
        {
            if (s_line[0] == 0)
            {
                continue;
            }
            if (gsm_is_urc(s_line))
            {
                gsm_dispatch_urc(s_line);
                continue;
            }
            strcpy(s_last_line, s_line);
            if (strstr(s_line, "+CMGS:") != NULL)
            {
                ret = 1;
                /* fall through and also collect the trailing OK/ERROR quietly */
            }
            if (strstr(s_line, "ERROR") != NULL)
            {
                return 0;
            }
            if (ret && strstr(s_line, "OK") != NULL)
            {
                return 1;
            }
        }
        gsm_delay_ms(1);
    }
    return ret;
}

/**
 * @brief       Read and delete one stored SMS, then add it to the RAM inbox and
 *              run the remote-control command parser.
 */
static void gsm_sms_read(int idx)
{
    char  cmd[48];
    char  sender[GSM_NUM_MAX_LEN + 1] = {0};
    char  msg[GSM_SMS_MAX_LEN + 1]    = {0};
    char  mem[8];
    int   got_head = 0, got_msg = 0, switched = 0;
    uint8_t log_prev;
    uint32_t left;

    /* the CPMS/CMGR/CMGD exchange is housekeeping: keep it out of the AT console */
    log_prev = s_log_silent;
    s_log_silent = 1;

    /* read from the storage the +CMTI indication pointed to (SIM7600 may
     * deliver into "ME" while the default read storage is "SM") */
    strcpy(mem, (const char *)s_pending_mem);
    s_pending_mem[0] = 0;
    if (mem[0] != 0)
    {
        snprintf(cmd, sizeof(cmd), "AT+CPMS=\"%s\"\r\n", mem);
        gsm_cmd(cmd, "OK", 1500);
        switched = 1;
    }

    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d\r\n", idx);
    gsm_puts(cmd);

    left = 8000;
    while (left-- > 0)
    {
        if (gsm_get_line(s_line, GSM_LINE_MAX))
        {
            if (s_line[0] == 0)
            {
                continue;
            }
            if (gsm_is_urc(s_line))
            {
                gsm_dispatch_urc(s_line);
                continue;
            }
            strcpy(s_last_line, s_line);
            if (strstr(s_line, "ERROR") != NULL)
            {
                break;
            }
            if (!strncmp(s_line, "+CMGR:", 6))
            {
                gsm_pick_sender(s_line, sender, sizeof(sender));
                got_head = 1;
                continue;
            }
            if (got_head && !got_msg && strcmp(s_line, "OK") != 0)
            {
                strncpy(msg, s_line, GSM_SMS_MAX_LEN);
                msg[GSM_SMS_MAX_LEN] = 0;
                got_msg = 1;
                continue;
            }
            if (got_head && strstr(s_line, "OK") != NULL)
            {
                break;
            }
        }
        gsm_delay_ms(1);
    }

    /* restore the default SMS storage so later reads/sweeps stay predictable */
    if (switched)
    {
        gsm_cmd("AT+CPMS=\"SM\",\"SM\",\"SM\"\r\n", "OK", 1500);
    }

    /* delete the stored copy, whatever happened */
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d\r\n", idx);
    gsm_cmd(cmd, "OK", 3000);

    s_log_silent = log_prev;

    gsm_ascii_clean(msg);               /* UCS-2/UTF-8 arrivals become plain ASCII */

    if (got_head && sender[0] != 0)
    {
        CPU_SR_ALLOC();
        uint32_t slot;

        snprintf(cmd, sizeof(cmd), "! SMS from %s, saved to inbox", sender);
        gsm_log_append(cmd);
        if (msg[0] != 0)
        {
            gsm_log_append(msg);                /* show what actually arrived */
        }

        CPU_CRITICAL_ENTER();
        slot = g_gsm_inbox_cnt % GSM_INBOX_MAX;
        strncpy(g_gsm_inbox[slot].num, sender, GSM_NUM_MAX_LEN);
        g_gsm_inbox[slot].num[GSM_NUM_MAX_LEN] = 0;
        strncpy(g_gsm_inbox[slot].msg, msg, GSM_SMS_MAX_LEN);
        g_gsm_inbox[slot].msg[GSM_SMS_MAX_LEN] = 0;
        g_gsm_inbox_cnt++;
        g_gsm_inbox_dirty++;
        CPU_CRITICAL_EXIT();

        gsm_cmd_exec(sender, msg);   /* remote control + auto reply */
    }
    else
    {
        gsm_log_append("! SMS read FAIL (check CPMS/storage)");
    }
}

void gsm_inbox_add(const char *num, const char *msg)
{
    CPU_SR_ALLOC();
    uint32_t slot;

    CPU_CRITICAL_ENTER();
    slot = g_gsm_inbox_cnt % GSM_INBOX_MAX;
    strncpy(g_gsm_inbox[slot].num, num, GSM_NUM_MAX_LEN);
    g_gsm_inbox[slot].num[GSM_NUM_MAX_LEN] = 0;
    strncpy(g_gsm_inbox[slot].msg, msg, GSM_SMS_MAX_LEN);
    g_gsm_inbox[slot].msg[GSM_SMS_MAX_LEN] = 0;
    gsm_ascii_clean(g_gsm_inbox[slot].msg);     /* keep the inbox display clean */
    g_gsm_inbox_cnt++;
    g_gsm_inbox_dirty++;
    CPU_CRITICAL_EXIT();
}

/**
 * @brief       Send the queued control-command reply, if any. Must be called
 *              from the GSM task context.
 */
static void gsm_serve_reply(void)
{
    char num[GSM_NUM_MAX_LEN + 1];
    char msg[GSM_SMS_MAX_LEN + 1];
    CPU_SR_ALLOC();

    if (!s_reply_req)
    {
        return;
    }
    CPU_CRITICAL_ENTER();
    strncpy(num, (const char *)s_reply_num, GSM_NUM_MAX_LEN);
    num[GSM_NUM_MAX_LEN] = 0;
    strncpy(msg, (const char *)s_reply_msg, GSM_SMS_MAX_LEN);
    msg[GSM_SMS_MAX_LEN] = 0;
    s_reply_req = 0;
    CPU_CRITICAL_EXIT();

    gsm_sms_send(num, msg);
}

/**
 * @brief       Fallback receive path: list all unread stored SMS and process
 *              them. Catches messages whose +CMTI indication never arrived
 *              (e.g. CNMI was rejected while the module was busy registering).
 *              The whole list is collected first and only processed after the
 *              final OK, so no AT command is ever sent in the middle of the
 *              +CMGL response.
 */
#define GSM_SWEEP_MAX   4                   /* max messages handled per sweep    */

static void gsm_sms_sweep(void)
{
    char     nums[GSM_SWEEP_MAX][GSM_NUM_MAX_LEN + 1];
    char     msgs[GSM_SWEEP_MAX][GSM_SMS_MAX_LEN + 1];
    int      idxs[GSM_SWEEP_MAX];
    int      cnt = 0;
    int      cur_idx = -1;
    char     cur_num[GSM_NUM_MAX_LEN + 1] = {0};
    char     cur_msg[GSM_SMS_MAX_LEN + 1] = {0};
    char     cmd[24];
    uint8_t  log_prev;
    int      i;
    uint32_t left;

    /* housekeeping: keep the sweep exchange out of the AT console */
    log_prev = s_log_silent;
    s_log_silent = 1;

    gsm_puts("AT+CMGL=\"REC UNREAD\"\r\n");

    left = 10000;
    while (left-- > 0)
    {
        if (gsm_get_line(s_line, GSM_LINE_MAX))
        {
            if (s_line[0] == 0)
            {
                continue;
            }
            if (gsm_is_urc(s_line))
            {
                gsm_dispatch_urc(s_line);
                continue;
            }
            strcpy(s_last_line, s_line);
            if (strstr(s_line, "ERROR") != NULL)
            {
                break;
            }
            if (!strncmp(s_line, "+CMGL:", 6))
            {
                /* a new entry starts: stash the previous one first */
                if (cur_idx >= 0 && cnt < GSM_SWEEP_MAX)
                {
                    idxs[cnt] = cur_idx;
                    strcpy(nums[cnt], cur_num);
                    strcpy(msgs[cnt], cur_msg);
                    cnt++;
                }
                cur_idx = atoi(s_line + 6);
                cur_num[0] = 0;
                cur_msg[0] = 0;
                gsm_pick_sender(s_line, cur_num, sizeof(cur_num));
                continue;
            }
            if (cur_idx >= 0 && strcmp(s_line, "OK") != 0)
            {
                strncpy(cur_msg, s_line, GSM_SMS_MAX_LEN);
                cur_msg[GSM_SMS_MAX_LEN] = 0;
                if (cnt < GSM_SWEEP_MAX)
                {
                    idxs[cnt] = cur_idx;
                    strcpy(nums[cnt], cur_num);
                    strcpy(msgs[cnt], cur_msg);
                    cnt++;
                }
                cur_idx = -1;
                cur_num[0] = 0;
                cur_msg[0] = 0;
                continue;
            }
            if (strstr(s_line, "OK") != NULL)
            {
                break;
            }
        }
        gsm_delay_ms(1);
    }

    if (cur_idx >= 0 && cnt < GSM_SWEEP_MAX)    /* trailing entry without text line */
    {
        idxs[cnt] = cur_idx;
        strcpy(nums[cnt], cur_num);
        strcpy(msgs[cnt], cur_msg);
        cnt++;
    }

    /* response finished: now inbox + remote control + delete, entry by entry */
    for (i = 0; i < cnt; i++)
    {
        if (nums[i][0] != 0)
        {
            gsm_inbox_add(nums[i], msgs[i]);
            gsm_cmd_exec(nums[i], msgs[i]);  /* may queue a reply ... */
            gsm_serve_reply();               /* ... send it right away */
        }
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d\r\n", idxs[i]);
        gsm_cmd(cmd, "OK", 3000);
    }

    s_log_silent = log_prev;
}

/**
 * @brief       Queue an auto-reply SMS.
 */
static void gsm_reply_queue(const char *num, const char *msg)
{
    CPU_SR_ALLOC();
    CPU_CRITICAL_ENTER();
    strncpy((char *)s_reply_num, num, GSM_NUM_MAX_LEN);
    s_reply_num[GSM_NUM_MAX_LEN] = 0;
    strncpy((char *)s_reply_msg, msg, GSM_SMS_MAX_LEN);
    s_reply_msg[GSM_SMS_MAX_LEN] = 0;
    s_reply_req = 1;
    CPU_CRITICAL_EXIT();
}

/******************************************************************************************/
/* Remote control command parser (SMS -> LED / BEEP) */

/**
 * @brief       Execute a control command from an SMS and reply to the sender.
 *              Commands (case-insensitive):
 *                LED0 ON | LED0 OFF | LED1 ON | LED1 OFF | LED ON | LED OFF
 *                BEEP ON | BEEP OFF | ALL ON | ALL OFF | STATUS
 * @param       num : sender number
 * @param       msg : command text
 * @retval      none
 */
static void gsm_cmd_exec(const char *num, const char *msg)
{
    char  buf[GSM_SMS_MAX_LEN + 1];
    char  reply[GSM_SMS_MAX_LEN + 1];
    int   i;
    int   led0 = -1, led1 = -1, beep = -1, ok = 1, status_only = 0;

    strncpy(buf, msg, GSM_SMS_MAX_LEN);
    buf[GSM_SMS_MAX_LEN] = 0;
    gsm_ascii_clean(buf);                       /* UCS-2/UTF-8 arrivals -> plain ASCII */
    for (i = 0; buf[i]; i++)
    {
        buf[i] = (char)toupper((unsigned char)buf[i]);
    }
    while (i > 0 && (buf[i - 1] == '\r' || buf[i - 1] == '\n' || buf[i - 1] == ' '))
    {
        buf[--i] = 0;
    }

    if      (!strcmp(buf, "LED0 ON"))   { led0 = 1;  }
    else if (!strcmp(buf, "LED0 OFF"))  { led0 = 0;  }
    else if (!strcmp(buf, "LED1 ON"))   { led1 = 1;  }
    else if (!strcmp(buf, "LED1 OFF"))  { led1 = 0;  }
    else if (!strcmp(buf, "LED ON"))    { led0 = 1;  led1 = 1;  }
    else if (!strcmp(buf, "LED OFF"))   { led0 = 0;  led1 = 0;  }
    else if (!strcmp(buf, "BEEP ON"))   { beep = 1;  }
    else if (!strcmp(buf, "BEEP OFF"))  { beep = 0;  }
    else if (!strcmp(buf, "ALL ON"))    { led0 = 1;  led1 = 1;  beep = 1;  }
    else if (!strcmp(buf, "ALL OFF"))   { led0 = 0;  led1 = 0;  beep = 0;  }
    else if (!strcmp(buf, "STATUS"))    { status_only = 1; }
    else                                { ok = 0; }

    if (ok)
    {
        /* on-board LEDs are active-low (LEDx(1) = off), the buzzer active-high */
        if (led0 >= 0) { LED0((GPIO_PinState)!led0); }
        if (led1 >= 0) { LED1((GPIO_PinState)!led1); }
        if (beep >= 0) { BEEP((GPIO_PinState)beep); }

        /* short audible feedback for LED / STATUS commands (not for BEEP itself) */
        if (beep < 0)
        {
            BEEP(1);
            gsm_delay_ms(100);
            BEEP(0);
        }

        snprintf(reply, sizeof(reply), "OK LED0:%d LED1:%d BEEP:%d",
                 (int)(led0 >= 0 ? led0 : (HAL_GPIO_ReadPin(LED0_GPIO_PORT, LED0_GPIO_PIN) == GPIO_PIN_RESET)),
                 (int)(led1 >= 0 ? led1 : (HAL_GPIO_ReadPin(LED1_GPIO_PORT, LED1_GPIO_PIN) == GPIO_PIN_RESET)),
                 (int)(beep >= 0 ? beep : (HAL_GPIO_ReadPin(BEEP_GPIO_PORT, BEEP_GPIO_PIN) == GPIO_PIN_SET)));
    }
    else
    {
        snprintf(reply, sizeof(reply),
                 "ERR CMD. USE: LED0/LED1/BEEP/ALL ON|OFF, STATUS");
    }
    (void)status_only;

    gsm_reply_queue(num, reply);
}

/******************************************************************************************/
/* Call handling */

static void gsm_do_dial(const char *num)
{
    char cmd[40];
    int  r;

    snprintf(cmd, sizeof(cmd), "ATD%s;\r\n", num);
    r = gsm_cmd(cmd, "OK", 8000);
    if (r == 1)
    {
        g_gsm_call_state  = GSM_CALL_DIALING;
        g_gsm_call_fail   = GSM_DIAL_FAIL_NONE;
        strncpy((char *)g_gsm_call_num, num, GSM_NUM_MAX_LEN);
        g_gsm_call_num[GSM_NUM_MAX_LEN] = 0;
        g_gsm_call_evt++;
    }
    else
    {
        /* show the real cause on the UI instead of failing silently */
        g_gsm_call_state  = GSM_CALL_IDLE;
        g_gsm_call_num[0] = 0;
        g_gsm_call_fail   = (r == -1) ? GSM_DIAL_FAIL_ERROR : GSM_DIAL_FAIL_TIMEOUT;
        g_gsm_call_evt++;
    }
}

static void gsm_do_hangup(void)
{
    gsm_cmd("ATH\r\n", "OK", 3000);
    g_gsm_call_state = GSM_CALL_IDLE;
    g_gsm_call_num[0] = 0;
    g_gsm_call_evt++;
}

static void gsm_do_answer(void)
{
    if (gsm_cmd("ATA\r\n", "OK", 3000) == 1)
    {
        g_gsm_call_state = GSM_CALL_ACTIVE;
        g_gsm_call_evt++;
    }
}

/******************************************************************************************/
/* Module initialization */

/**
 * @brief       One-shot module configuration after the module answered "AT".
 *              Only the echo-off and text-mode switches gate readiness; the
 *              remaining commands are best effort (unsupported on some module
 *              firmwares and must never block calling/SMS).
 * @retval      1 if the essential config was accepted
 */
static int gsm_module_config(void)
{
    int essential_ok = 1;

    if (gsm_cmd("ATE0\r\n", "OK", 1000) != 1)       essential_ok = 0;  /* echo off      */
    gsm_cmd("AT+CPIN?\r\n", NULL, 1000);                                /* SIM state     */
    if (gsm_cmd("AT+CMGF=1\r\n", "OK", 1000) != 1)  essential_ok = 0;  /* SMS text mode */

    /* best effort: failures are logged but ignored */
    gsm_cmd("AT+CSCS=\"GSM\"\r\n", "OK", 1000);
    if (gsm_cmd("AT+CNMI=2,1,0,0,0\r\n", "OK", 1000) == 1)
    {
        s_cnmi_ok = 1;
    }
    gsm_cmd("AT+CLIP=1\r\n", "OK", 1000);
    gsm_cmd("AT+CPMS=\"SM\",\"SM\",\"SM\"\r\n", "OK", 1000);

    gsm_cmd("AT+CSQ\r\n", "OK", 1500);                   /* initial signal quality  */
    if (strncmp(s_last_line, "+CSQ:", 5) == 0)
    {
        int rssi = atoi(s_last_line + 5);
        g_gsm_signal = (rssi >= 0 && rssi <= 31) ? (uint8_t)rssi : 99;
    }
    if (gsm_cmd("AT+CREG?\r\n", "OK", 1500) == 1 &&
        strncmp(s_last_line, "+CREG:", 6) == 0)
    {
        const char *p = strchr(s_last_line, ',');
        gsm_reg_update((p != NULL) ? atoi(p + 1) : atoi(s_last_line + 6));
    }
    return essential_ok;
}

/**
 * @brief       Try to bring the module up (auto-baud sync + config), with retries.
 */
static void gsm_init(void)
{
    int tries = 0;

    while (tries < 10 && g_gsm_ready == 0)
    {
        if (gsm_cmd("AT\r\n", "OK", 600) == 1)
        {
            g_gsm_ready = gsm_module_config();
            break;
        }
        gsm_delay_ms(500);
        tries++;
    }
    /* poll loop runs every 10 ms: 200 ticks = 2 s between module retries */
    s_init_retry_tmr = 200;
}

/******************************************************************************************/
/* Request API (called from the UI task) */

void gsm_req_dial(const char *num)
{
    CPU_SR_ALLOC();
    CPU_CRITICAL_ENTER();
    strncpy((char *)s_req_dial_num, num, GSM_NUM_MAX_LEN);
    s_req_dial_num[GSM_NUM_MAX_LEN] = 0;
    s_req_dial = 1;
    CPU_CRITICAL_EXIT();
}

void gsm_req_hangup(void)
{
    s_req_hang = 1;
}

void gsm_req_answer(void)
{
    s_req_ans = 1;
}

void gsm_req_sms(const char *num, const char *msg)
{
    CPU_SR_ALLOC();
    CPU_CRITICAL_ENTER();
    strncpy((char *)s_req_sms_num, num, GSM_NUM_MAX_LEN);
    s_req_sms_num[GSM_NUM_MAX_LEN] = 0;
    strncpy((char *)s_req_msg, msg, GSM_SMS_MAX_LEN);
    s_req_msg[GSM_SMS_MAX_LEN] = 0;
    s_req_sms = 1;
    CPU_CRITICAL_EXIT();
}

/**
 * @brief       Queue a raw AT command (from the test page). CRLF is appended
 *              automatically if missing.
 */
void gsm_req_at(const char *cmd)
{
    CPU_SR_ALLOC();
    CPU_CRITICAL_ENTER();
    strncpy((char *)s_req_at_cmd, cmd, sizeof(s_req_at_cmd) - 1);
    s_req_at_cmd[sizeof(s_req_at_cmd) - 1] = 0;
    s_req_at = 1;
    CPU_CRITICAL_EXIT();
}

/**
 * @brief       Copy the AT console log. If it does not fit, only the newest tail
 *              (aligned to a line start) is copied.
 */
void gsm_at_get_log(char *buf, int max)
{
    CPU_SR_ALLOC();
    uint16_t len;
    uint16_t start = 0;

    if (max <= 0)
    {
        return;
    }

    CPU_CRITICAL_ENTER();
    len = s_at_log_len;
    if (len >= (uint16_t)max)
    {
        start = (uint16_t)(len - (uint16_t)max + 1);
        while (start < len && s_at_log[start] != '\n')
        {
            start++;
        }
        if (start < len)
        {
            start++;
        }
    }
    memcpy(buf, (const void *)(s_at_log + start), (uint16_t)(len - start));
    buf[len - start] = 0;
    CPU_CRITICAL_EXIT();
}

/**
 * @brief       Clear the AT console log.
 */
void gsm_at_clear_log(void)
{
    CPU_SR_ALLOC();
    CPU_CRITICAL_ENTER();
    s_at_log_len = 0;
    s_at_log[0] = 0;
    g_gsm_at_log_dirty++;
    CPU_CRITICAL_EXIT();
}

uint8_t gsm_get_sms_result(void)
{
    return g_gsm_sms_result;
}

void gsm_format_call(char *state_txt, char *num_txt)
{
    CPU_SR_ALLOC();
    uint8_t st;

    CPU_CRITICAL_ENTER();
    st = g_gsm_call_state;
    strncpy(num_txt, (const char *)g_gsm_call_num, GSM_NUM_MAX_LEN);
    num_txt[GSM_NUM_MAX_LEN] = 0;
    CPU_CRITICAL_EXIT();

    switch (st)
    {
    case GSM_CALL_DIALING:  strcpy(state_txt, "DIALING");  break;
    case GSM_CALL_INCOMING: strcpy(state_txt, "INCOMING");  break;
    case GSM_CALL_ACTIVE:   strcpy(state_txt, "TALKING");   break;
    default:                strcpy(state_txt, "READY");     break;
    }
}

/******************************************************************************************/
/* Main poll (called periodically from gsm_task) */

/**
 * @brief       Housekeeping: drain URCs, read pending SMS, serve UI requests,
 *              refresh registration / signal, retry module init if needed.
 */
static void gsm_poll(void)
{
    gsm_drain_lines();

    /* UI requests are served even when the module is not fully configured yet,
     * so a refused optional config can never swallow them silently. */
    if (s_req_hang)
    {
        s_req_hang = 0;
        gsm_do_hangup();
    }
    if (s_req_ans)
    {
        s_req_ans = 0;
        gsm_do_answer();
    }
    if (s_req_dial)
    {
        char num[GSM_NUM_MAX_LEN + 1];
        CPU_SR_ALLOC();
        CPU_CRITICAL_ENTER();
        strncpy(num, (const char *)s_req_dial_num, GSM_NUM_MAX_LEN);
        num[GSM_NUM_MAX_LEN] = 0;
        s_req_dial = 0;
        CPU_CRITICAL_EXIT();
        gsm_do_dial(num);
    }
    if (s_req_at)
    {
        char cmd[96];
        CPU_SR_ALLOC();
        CPU_CRITICAL_ENTER();
        strncpy(cmd, (const char *)s_req_at_cmd, sizeof(cmd) - 3);
        cmd[sizeof(cmd) - 3] = 0;
        s_req_at = 0;
        CPU_CRITICAL_EXIT();

        if (strlen(cmd) > 0)
        {
            if (strstr(cmd, "\n") == NULL)
            {
                strcat(cmd, "\r\n");
            }
            gsm_cmd(cmd, "OK", 8000);   /* cmd + all response lines go to the log */
        }
    }
    if (s_req_sms)
    {
        char num[GSM_NUM_MAX_LEN + 1];
        char msg[GSM_SMS_MAX_LEN + 1];
        CPU_SR_ALLOC();
        CPU_CRITICAL_ENTER();
        strncpy(num, (const char *)s_req_sms_num, GSM_NUM_MAX_LEN);
        num[GSM_NUM_MAX_LEN] = 0;
        strncpy(msg, (const char *)s_req_msg, GSM_SMS_MAX_LEN);
        msg[GSM_SMS_MAX_LEN] = 0;
        s_req_sms = 0;
        CPU_CRITICAL_EXIT();

        g_gsm_sms_result = GSM_SMS_ST_SENDING;
        if (gsm_sms_send(num, msg))
        {
            g_gsm_sms_result = GSM_SMS_ST_OK;
            gsm_inbox_add(num, msg);   /* keep a local copy of sent SMS */
        }
        else
        {
            g_gsm_sms_result = GSM_SMS_ST_FAIL;
        }
    }

    if (g_gsm_ready)
    {
        /* new SMS waiting in module memory */
        if (s_pending_sms >= 0)
        {
            int idx = s_pending_sms;
            s_pending_sms = -1;
            gsm_sms_read(idx);
        }

        /* queued control-command reply */
        gsm_serve_reply();

        /* CNMI is required for +CMTI push delivery: retry until accepted
         * (the module may refuse it while busy registering at boot).
         * Silent: automatic retries must not pollute the AT console. */
        if (!s_cnmi_ok)
        {
            if (s_cnmi_retry_tmr == 0)
            {
                uint8_t log_prev;

                s_cnmi_retry_tmr = 500;          /* retry every 5 s */
                log_prev = s_log_silent;
                s_log_silent = 1;
                if (gsm_cmd("AT+CNMI=2,1,0,0,0\r\n", "OK", 1500) == 1)
                {
                    s_cnmi_ok = 1;
                }
                s_log_silent = log_prev;
            }
            else
            {
                s_cnmi_retry_tmr--;
            }
        }

        /* fallback receive path: sweep unread storage every 30 s (idle only) */
        if (g_gsm_call_state == GSM_CALL_IDLE)
        {
            if (s_cmgl_tmr == 0)
            {
                s_cmgl_tmr = 3000;
                gsm_sms_sweep();
            }
            else
            {
                s_cmgl_tmr--;
            }
        }

        /* periodic signal quality + registration poll (silent, no log spam) */
        if (s_csq_tmr == 0)
        {
            s_csq_tmr = 6000;                    /* every 60 s */
            s_log_silent = 1;
            if (gsm_cmd("AT+CSQ\r\n", "OK", 2000) == 1 &&
                strncmp(s_last_line, "+CSQ:", 5) == 0)
            {
                int rssi = atoi(s_last_line + 5);
                g_gsm_signal = (rssi >= 0 && rssi <= 31) ? (uint8_t)rssi : 99;
            }
            if (gsm_cmd("AT+CREG?\r\n", "OK", 2000) == 1 &&
                strncmp(s_last_line, "+CREG:", 6) == 0)
            {
                const char *p = strchr(s_last_line, ',');
                gsm_reg_update((p != NULL) ? atoi(p + 1) : atoi(s_last_line + 6));
            }
            s_log_silent = 0;
        }
        else
        {
            s_csq_tmr--;
        }
    }
    else
    {
        /* module missing / SIM error: retry "AT" every 2 s (silent: the
         * console is reserved for commands the user actually presses) */
        if (s_init_retry_tmr == 0)
        {
            uint8_t log_prev = s_log_silent;

            s_log_silent = 1;
            gsm_init();
            s_log_silent = log_prev;
        }
        else
        {
            s_init_retry_tmr--;
        }
    }
}

/******************************************************************************************/
/* uC/OS-III task entry */

/**
 * @brief       GSM engine task.
 */
void gsm_task(void *p_arg)
{
    OS_ERR err;
    p_arg = p_arg;

    /* silent: automatic init traffic never appears in the AT console */
    s_log_silent = 1;
    gsm_rx_flush();
    gsm_init();
    s_log_silent = 0;

    while (1)
    {
        gsm_poll();
        OSTimeDly(10, OS_OPT_TIME_DLY, &err);   /* poll every 10 ms */
    }
}

/******************************************************************************************/
