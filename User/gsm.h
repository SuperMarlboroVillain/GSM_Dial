/**
 ****************************************************************************************************
 * @file        gsm.h
 * @brief       GSM module (SIM800C/SIM900 series) driver over USART1, AT command engine.
 *              Works together with uC/OS-III. Provides arbitrary dialing, SMS send/receive
 *              and SMS remote control of the on-board LED and BEEP.
 * @note        ASCII only. SMS text mode with "GSM" charset, keep SMS content in English.
 ****************************************************************************************************
 */

#ifndef __GSM_H
#define __GSM_H

#include "./SYSTEM/sys/sys.h"

/******************************************************************************************/
/* Configuration */

#define GSM_SMS_MAX_LEN     140                 /* max chars of one SMS text   */
#define GSM_NUM_MAX_LEN     20                  /* max chars of a phone number */
#define GSM_INBOX_MAX       8                   /* max SMS kept in RAM inbox   */

/* call states */
#define GSM_CALL_IDLE       0
#define GSM_CALL_DIALING    1
#define GSM_CALL_INCOMING   2
#define GSM_CALL_ACTIVE     3

/* sms send result codes */
#define GSM_SMS_ST_NONE     0
#define GSM_SMS_ST_SENDING  1
#define GSM_SMS_ST_OK       2
#define GSM_SMS_ST_FAIL     3

/* last dial failure cause */
#define GSM_DIAL_FAIL_NONE      0
#define GSM_DIAL_FAIL_ERROR     1   /* module answered ERROR (no reg/no SIM/PIN) */
#define GSM_DIAL_FAIL_TIMEOUT   2   /* module did not answer the ATD command     */

/* network registration state (from AT+CREG?) */
#define GSM_REG_UNKNOWN         0
#define GSM_REG_OK              1   /* registered (home 1 or roaming 5)          */
#define GSM_REG_OFFLINE         2   /* not registered (stat 0)                   */
#define GSM_REG_SEARCHING       3   /* searching / unknown (stat 2 / 4)          */
#define GSM_REG_DENIED          4   /* registration denied (stat 3)              */

/******************************************************************************************/
/* Types */

typedef struct
{
    char num[GSM_NUM_MAX_LEN + 1];              /* sender number       */
    char msg[GSM_SMS_MAX_LEN + 1];              /* message text        */
} gsm_sms_t;

/******************************************************************************************/
/* Shared state (written by gsm_task, read by the UI task) */

extern volatile uint8_t  g_gsm_ready;           /* 1: module initialized OK        */
extern volatile uint8_t  g_gsm_signal;          /* CSQ rssi 0..31 (99 = unknown)   */
extern volatile uint8_t  g_gsm_call_state;      /* GSM_CALL_x                      */
extern volatile uint8_t  g_gsm_call_evt;        /* bumped on every call state change */
extern volatile uint8_t  g_gsm_call_fail;       /* GSM_DIAL_FAIL_x                 */
extern volatile uint8_t  g_gsm_reg;             /* GSM_REG_x                       */
extern volatile char     g_gsm_call_num[GSM_NUM_MAX_LEN + 1];
extern volatile uint8_t  g_gsm_sms_result;      /* GSM_SMS_ST_x                    */
extern volatile uint32_t g_gsm_inbox_cnt;       /* total SMS received (ring index) */
extern volatile uint32_t g_gsm_inbox_dirty;     /* bumped whenever inbox changes   */
extern volatile uint32_t g_gsm_at_log_dirty;    /* bumped whenever the AT log grows */
extern gsm_sms_t         g_gsm_inbox[GSM_INBOX_MAX];

/******************************************************************************************/
/* Public API */

void     gsm_task(void *p_arg);                 /* uC/OS-III task entry, never returns */

/* request functions, safe to call from another task (UI) */
void     gsm_req_dial(const char *num);
void     gsm_req_hangup(void);
void     gsm_req_answer(void);
void     gsm_req_sms(const char *num, const char *msg);
void     gsm_req_at(const char *cmd);           /* raw AT command for the test page */

/* helpers for the UI task */
uint8_t  gsm_get_sms_result(void);
void     gsm_format_call(char *state_txt, char *num_txt);   /* copies under critical section */
void     gsm_at_get_log(char *buf, int max);    /* copy AT response log (tail if too long) */
void     gsm_at_clear_log(void);                /* clear the AT response log */

#endif
