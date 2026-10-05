/**
 ****************************************************************************************************
 * @file        usart.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.2
 * @date        2026-09-20
 * @brief       串口初始化代码(一般是串口1)，支持printf
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 探索者 F407开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 * 修改说明
 * V1.0 20211014
 * 第一次发布
 * V1.1 20230605
 * 删除USART_UX_IRQHandler()函数的超时处理和修改HAL_UART_RxCpltCallback()
 * V1.2 20260920
 * RX路径改为环形缓冲区接收: ISR逐字节入环, 任务侧按行取出, 背靠背到达的
 * 多条响应行不再丢字节(原单缓冲行接收在完成行被取走前会丢弃后续字节)。
 * 新增 usart_get_line() / usart_rx_pop() / usart_rx_flush() 接口。
 ****************************************************************************************************
 */

#ifndef _USART_H
#define _USART_H

#include "stdio.h"
#include "./SYSTEM/sys/sys.h"

/*******************************************************************************************************/
/* 引脚 和 串口 定义 
 * 默认是针对USART1的.
 * 注意: 通过修改这12个宏定义,可以支持USART1~UART7任意一个串口.
 */

#define USART_TX_GPIO_PORT              GPIOA
#define USART_TX_GPIO_PIN               GPIO_PIN_9
#define USART_TX_GPIO_AF                GPIO_AF7_USART1
#define USART_TX_GPIO_CLK_ENABLE()      do{ __HAL_RCC_GPIOA_CLK_ENABLE(); }while(0)   /* 发送引脚时钟使能 */

#define USART_RX_GPIO_PORT              GPIOA
#define USART_RX_GPIO_PIN               GPIO_PIN_10
#define USART_RX_GPIO_AF                GPIO_AF7_USART1
#define USART_RX_GPIO_CLK_ENABLE()      do{ __HAL_RCC_GPIOA_CLK_ENABLE(); }while(0)   /* 接收引脚时钟使能 */

#define USART_UX                        USART1
#define USART_UX_IRQn                   USART1_IRQn
#define USART_UX_IRQHandler             USART1_IRQHandler
#define USART_UX_CLK_ENABLE()           do{ __HAL_RCC_USART1_CLK_ENABLE(); }while(0)  /* USART1 时钟使能 */

/*******************************************************************************************************/

#define USART_EN_RX     1                       /* 使能（1）/禁止（0）串口1接收 */

extern UART_HandleTypeDef g_uart1_handle;       /* UART句柄 */

void usart_init(uint32_t baudrate);             /* 串口初始化函数 */

/* 行接收接口 (ISR入环, 任务侧取出, 二者均可安全调用) */
int  usart_get_line(char *buf, int max);        /* 取出一行(去掉CRLF): 1=取到一行(可能为空), 0=还没有完整行 */
int  usart_rx_pop(uint8_t *b);                  /* 取出一个原始字节: 1=取到, 0=缓冲为空 */
void usart_rx_flush(void);                      /* 丢弃全部已缓冲的接收数据 */

#endif
