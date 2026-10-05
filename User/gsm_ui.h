/**
 ****************************************************************************************************
 * @file        gsm_ui.h
 * @brief       emWin multi-page UI for the GSM dialer: DIAL / SMS / INBOX pages plus a
 *              status bar. Must be driven from a single task: call gsm_ui_create() once
 *              after GUI_Init(), then call gsm_ui_update() periodically.
 ****************************************************************************************************
 */

#ifndef __GSM_UI_H
#define __GSM_UI_H

void gsm_ui_create(void);       /* create the dialog, call once */
void gsm_ui_update(void);       /* refresh dynamic widgets, call every ~10-20 ms */

#endif
