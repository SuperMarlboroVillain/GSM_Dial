#include "emwin_demo.h"
#include "./SYSTEM/usart/usart.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "math.h"

/*****************************************************************************************************/
/*EMWIN*/
#include "GUI.h"
#include "WM.h"
#include "DIALOG.h"
/*****************************************************************************************************/
/*uC-OS3*/
#include "os.h"
#include "cpu.h"

#include"./SYSTEM/DELAY/DELAY.h"

void emwin_main(void) 
{
{
	
		int i = 0;
		for(i=0;i<=2;i++)
		{
		
		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)"A",1, 1000);//发送数据
		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成4
		
		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)"T",1, 1000);//发送数据
		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成
		
		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)"\r",1, 1000);//发送数据
		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成
		
		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)"\n",1, 1000);//发送数据
		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成
	
			
		delay_ms(100);
		}
	
	}

//	{
//		unsigned char i = 0;
//		unsigned char phone_num[] = "15947072669;\r\n";
//		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)"A",1, 1000);//发送数据
//		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成
//		
//		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)"T",1, 1000);//发送数据
//		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成
//		
//		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)"D",1, 1000);//发送数据
//		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成
//		 
//		for (i=0;i<14;i++)
//		{
//			
//		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)&phone_num[i],1, 1000);//发送数据
//		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成
//			
//		
//	}
//	}

//
{
char atd[18] = "ATD";
	char phone_num[12] = "18810232294";
	char rear[4] = ";\r\n";
char i = 0;
strcat(atd,phone_num);
strcat(atd,rear);
	
	for (i=0;i<14;i++)
		{
			
		HAL_UART_Transmit(&g_uart1_handle,(uint8_t *)&phone_num[i],1, 1000);//发送数据
		while(__HAL_UART_GET_FLAG(&g_uart1_handle, UART_FLAG_TC) != SET);//等待发送完成
			
		
	}



}
}




