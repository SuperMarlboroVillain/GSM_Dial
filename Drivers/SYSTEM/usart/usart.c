/**
 ****************************************************************************************************
 * @file        usart.c
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
 * RX路径改为环形缓冲区接收: ISR只负责把字节压入环形缓冲(满时丢最新字节),
 * 任务侧通过 usart_get_line() 按 CRLF 切行取出。原单缓冲行接收在"一行已完成、
 * 尚未被取走"期间会丢弃后续到达的所有字节, 115200波特率下模块连续多行响应
 * (如 +CMGR: 头/正文/OK) 的行首字节必丢, 导致短信接收等解析失败。
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"


/* 如果使用os,则包括下面的头文件即可 */
#if SYS_SUPPORT_OS
#include "os.h"                               /* os 使用 */
#endif
#include "cpu.h"                              /* CPU_CRITICAL_ENTER/EXIT */

/******************************************************************************************/
/* 加入以下代码, 支持printf函数, 而不需要选择use MicroLIB */

#if 1
#if (__ARMCC_VERSION >= 6010050)                    /* 使用AC6编译器时 */
__asm(".global __use_no_semihosting\n\t");          /* 声明不使用半主机模式 */
__asm(".global __ARM_use_no_argv \n\t");            /* AC6下需要声明main函数为无参数格式，否则部分例程可能出现半主机模式 */

#else
/* 使用AC5编译器时, 要在这里定义__FILE 和 不使用半主机模式 */
#pragma import(__use_no_semihosting)

struct __FILE
{
    int handle;
    /* Whatever you require here. If you are only using stdio for
     * console output OR input, then no file handling is required. */
};

#endif

/* 不使用半主机模式，至少需要重定义_ttywrch\_sys_exit\_sys_command_string函数,以同时兼容AC6和AC5模式 */
int _ttywrch(int ch)
{
    ch = ch;
    return ch;
}

/* 定义_sys_exit()以避免使用半主机模式 */
void _sys_exit(int x)
{
    x = x;
}

char *_sys_command_string(char *cmd, int len)
{
    return NULL;
}

/* FILE 在 stdio.h里面定义. */
FILE __stdout;

/* 重定义fputc函数, printf函数最终会通过调用fputc输出字符串到串口 */
int fputc(int ch, FILE *f)
{
    while ((USART1->SR & 0X40) == 0);               /* 等待上一个字符发送完成 */

    USART1->DR = (uint8_t)ch;                       /* 将要发送的字符 ch 写入到DR寄存器 */
    return ch;
}
#endif
/***********************************************END*******************************************/

#if USART_EN_RX                                     /* 如果使能了接收 */

#define USART_RX_RING_SIZE   1024                   /* 接收环形缓冲大小(2的幂), 115200波特率下约90ms余量 */
#define RXBUFFERSIZE         1                      /* HAL库单字节接收缓冲 */

/* 接收环形缓冲: ISR压入(头部), 任务侧弹出(尾部) */
static volatile uint8_t  s_rx_ring[USART_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0;             /* 写索引(ISR) */
static volatile uint16_t s_rx_tail = 0;             /* 读索引(任务) */

uint8_t g_rx_buffer[RXBUFFERSIZE];                  /* HAL库使用的串口接收缓冲 */

UART_HandleTypeDef g_uart1_handle;                  /* UART句柄 */


/**
 * @brief       串口X初始化函数
 * @param       baudrate: 波特率, 根据自己需要设置波特率值
 * @note        注意: 必须设置正确的时钟源, 否则串口波特率就会设置异常.
 *              这里的USART的时钟源在sys_stm32_clock_init()函数中已经设置过了.
 * @retval      无
 */
void usart_init(uint32_t baudrate)
{
    g_uart1_handle.Instance = USART_UX;                         /* USART1 */
    g_uart1_handle.Init.BaudRate = baudrate;                    /* 波特率 */
    g_uart1_handle.Init.WordLength = UART_WORDLENGTH_8B;        /* 字长为8位数据格式 */
    g_uart1_handle.Init.StopBits = UART_STOPBITS_1;             /* 一个停止位 */
    g_uart1_handle.Init.Parity = UART_PARITY_NONE;              /* 无奇偶校验位 */
    g_uart1_handle.Init.HwFlowCtl = UART_HWCONTROL_NONE;        /* 无硬件流控 */
    g_uart1_handle.Init.Mode = UART_MODE_TX_RX;                 /* 收发模式 */
    HAL_UART_Init(&g_uart1_handle);                             /* HAL_UART_Init()会使能UART1 */

    /* 该函数会开启接收中断：标志位UART_IT_RXNE，并且设置接收缓冲以及接收缓冲接收最大数据量 */
    HAL_UART_Receive_IT(&g_uart1_handle, (uint8_t *)g_rx_buffer, RXBUFFERSIZE);
}

/**
 * @brief       UART底层初始化函数
 * @param       huart: UART句柄类型指针
 * @note        此函数会被HAL_UART_Init()调用
 *              完成时钟使能，引脚配置，中断配置
 * @retval      无
 */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio_init_struct;
    if(huart->Instance == USART_UX)                             /* 如果是串口1，进行串口1 MSP初始化 */
    {
        USART_UX_CLK_ENABLE();                                  /* USART1 时钟使能 */
        USART_TX_GPIO_CLK_ENABLE();                             /* 发送引脚时钟使能 */
        USART_RX_GPIO_CLK_ENABLE();                             /* 接收引脚时钟使能 */

        gpio_init_struct.Pin = USART_TX_GPIO_PIN;               /* TX引脚 */
        gpio_init_struct.Mode = GPIO_MODE_AF_PP;                /* 复用推挽输出 */
        gpio_init_struct.Pull = GPIO_PULLUP;                    /* 上拉 */
        gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;          /* 高速 */
        gpio_init_struct.Alternate = USART_TX_GPIO_AF;          /* 复用为USART1 */
        HAL_GPIO_Init(USART_TX_GPIO_PORT, &gpio_init_struct);   /* 初始化发送引脚 */

        gpio_init_struct.Pin = USART_RX_GPIO_PIN;               /* RX引脚 */
        gpio_init_struct.Alternate = USART_RX_GPIO_AF;          /* 复用为USART1 */
        HAL_GPIO_Init(USART_RX_GPIO_PORT, &gpio_init_struct);   /* 初始化接收引脚 */

#if USART_EN_RX
        HAL_NVIC_EnableIRQ(USART_UX_IRQn);                      /* 使能USART1中断通道 */
        HAL_NVIC_SetPriority(USART_UX_IRQn, 3, 3);              /* 抢占优先级3，子优先级3 */
#endif
    }
}

/**
 * @brief       Rx传输回调函数
 * @param       huart: UART句柄类型指针
 * @note        只做一件事: 把收到的字节压入环形缓冲(满则丢弃最新字节)。
 *              行切分留给任务侧的 usart_get_line(), 无论响应行到达多快都不会丢字节。
 * @retval      无
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if(huart->Instance == USART_UX)             /* 如果是串口1 */
    {
        uint16_t next = (uint16_t)((s_rx_head + 1u) & (USART_RX_RING_SIZE - 1u));

        if (next != s_rx_tail)                  /* 环形缓冲未满: 存入字节 */
        {
            s_rx_ring[s_rx_head] = g_rx_buffer[0];
            s_rx_head = next;
        }
        /* 满了则丢弃最新字节(任务侧取数周期约1ms, 实际几乎不可能满) */

        HAL_UART_Receive_IT(&g_uart1_handle, (uint8_t *)g_rx_buffer, RXBUFFERSIZE);
    }
}

/**
 * @brief       从环形缓冲取出一行(以\n结尾, 去掉\r\n)。
 * @param       buf : 目标缓冲
 * @param       max : 目标缓冲大小
 * @retval      1: 取到一行(可能为空行), 0: 还没有完整行(不消耗任何数据)
 * @note        行超长时截断保存, 剩余字符照常消耗丢弃, 不会破坏后续行的边界。
 */
int usart_get_line(char *buf, int max)
{
    CPU_SR_ALLOC();
    uint16_t tail;
    int n = 0;
    int found = 0;

    if (max <= 1)
    {
        return 0;
    }

    CPU_CRITICAL_ENTER();
    tail = s_rx_tail;
    while (tail != s_rx_head)
    {
        uint8_t b = s_rx_ring[tail];
        tail = (uint16_t)((tail + 1u) & (USART_RX_RING_SIZE - 1u));
        if (b == '\n')                          /* 行结束 */
        {
            found = 1;
            break;
        }
        if (b == '\r')
        {
            continue;
        }
        if (n < max - 1)
        {
            buf[n++] = (char)b;
        }
    }
    if (found)                                  /* 只有取到完整行才提交读索引 */
    {
        s_rx_tail = tail;
    }
    CPU_CRITICAL_EXIT();

    buf[n] = 0;
    return found;
}

/**
 * @brief       从环形缓冲取出一个原始字节(用于轮询 '>' 等不带换行的提示符)。
 * @param       b : 输出字节
 * @retval      1: 取到, 0: 缓冲为空
 */
int usart_rx_pop(uint8_t *b)
{
    CPU_SR_ALLOC();
    int got = 0;

    CPU_CRITICAL_ENTER();
    if (s_rx_tail != s_rx_head)
    {
        *b = s_rx_ring[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1u) & (USART_RX_RING_SIZE - 1u));
        got = 1;
    }
    CPU_CRITICAL_EXIT();

    return got;
}

/**
 * @brief       丢弃全部已缓冲的接收数据。
 */
void usart_rx_flush(void)
{
    CPU_SR_ALLOC();
    CPU_CRITICAL_ENTER();
    s_rx_tail = s_rx_head;
    CPU_CRITICAL_EXIT();
}

/**
 * @brief       串口1中断服务函数
 * @param       无
 * @retval      无
 */
void USART_UX_IRQHandler(void)
{ 
#if SYS_SUPPORT_OS                              /* 使用OS */
    OSIntEnter();    
#endif

    HAL_UART_IRQHandler(&g_uart1_handle);       /* 调用HAL库中断处理公用函数 */

#if SYS_SUPPORT_OS                              /* 使用OS */
    OSIntExit();
#endif
}

#endif
