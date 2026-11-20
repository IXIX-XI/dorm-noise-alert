#include "stm32f10x.h"                  // Device header
void LED_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA,ENABLE);
	
	
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin=GPIO_Pin_1|GPIO_Pin_2;
	GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	GPIO_SetBits(GPIOA,GPIO_Pin_1|GPIO_Pin_2);//初始化为高电平。LED不亮
}
void LED1_ON(void)//也可以只定义一个函数，一个参数指定哪个灯，另一个指定亮or not
{
	GPIO_ResetBits(GPIOA,GPIO_Pin_1);//低电平，接在PA1的LED亮
}
void LED1_OFF(void)
{
	GPIO_SetBits(GPIOA,GPIO_Pin_1);//高电平，接在PA1的LED灭
}

void LED2_ON(void)
{
	GPIO_ResetBits(GPIOA,GPIO_Pin_2);//低电平，接在PA2的LED亮
}
void LED2_OFF(void)
{
	GPIO_SetBits(GPIOA,GPIO_Pin_2);//高电平，接在PA2的LED灭
}