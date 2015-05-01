/*
 * OneWire.c
 *
 *  Created on: Apr 25, 2015
 *      Author: Dustin
 */


/*
 * Currently I'm working on implementing the state logic to handle the different state transistions and
 * doing things like reading the bus state 550us after reset (it's 70 us after master stops pulling down bus).
 *
 * With a solid interrupt driven library, the processor can go into sleep and get woken up to perform the next action,
 * if one wire sampling is needed.
 *
 * The below code generated some unexpected timing, so I'm working in timer to see how I can improve it.  Currently I'm using
 * the default crystal, which is the MSI at 2.097 MHz. HSI clock of 16 MHz is available and may be the best since  16,000,000/1,000,000 = 16 periods (1000000 is 1/1u)
 *
 * Currently the basic reset seems to mostly work, but it has inconsistent timings for the 240us phase.
 *
 */
#include "stm32l1xx.h"
#include "onewire.h"

typedef enum {
	onewire_reset = 0x00,
	onewire_write_0,
	onewire_write_1,
	onewire_read,
	onewire_single,
	onewire_done,
} onewire_state_t;


typedef struct {
	onewire_state_t state;
	uint16_t next_delay;
	onewire_level_t next_level;
} onewire_OWn_t;

GPIO_InitTypeDef gpioOW3;
NVIC_InitTypeDef nvicOW3;
TIM_TimeBaseInitTypeDef timerOW3;
onewire_OWn_t onewire_OW3;

volatile static TIM2_flag = 0;
void TIM2_IRQHandler()
{
    if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
        TIM2_flag = 1;
    }
}

void __inline__ init_TIM2_Change_Period(uint16_t period){
	//By disabling events, we can modify the Period register safely.
	//Once updated, re-enabling UEV events clears the existing counters,
	//giving us a clean slate to start from.
	TIM2->CNT = 0;
	TIM2->CR1 |= TIM_CR1_UDIS; //Disable UEV events
	TIM2->ARR = period;
	TIM2->CR1 &= ~TIM_CR1_UDIS; //Enable UEV events
}

void delayms(uint32_t msec){
	while (msec-- > 0){
		delayus(1000);
	}
}

void delayus(uint16_t usec){
	TIM2_flag = 0;

	init_TIM2_Change_Period(usec);
	while(TIM2_flag == 0){
		__WFI();
	};
}

void onewire_OW3Init(void){
	RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOC, ENABLE);



	/*-------------------------- GPIO Configuration ----------------------------*/
	gpioOW3.GPIO_Pin = GPIO_Pin_6;
	gpioOW3.GPIO_Mode = GPIO_Mode_OUT;
	gpioOW3.GPIO_OType = GPIO_OType_OD;  //open drain, 4.7k used as pull up
	gpioOW3.GPIO_PuPd = GPIO_PuPd_NOPULL;
	gpioOW3.GPIO_Speed = GPIO_Speed_40MHz;
	GPIO_Init(GPIOC, &gpioOW3);
}
void onewire_OW3Write(onewire_level_t state){
	if (state == onewire_high)
		GPIOC->BSRRH = GPIO_Pin_6;
	else
		GPIOC->BSRRL = GPIO_Pin_6;
}


void onewire_TIM2_Configuration(void){

	//Period between interrupts is (Period-1)/(16000000/(Prescaler-1))
	timerOW3.TIM_Prescaler = 31;
	timerOW3.TIM_CounterMode = TIM_CounterMode_Up;
	timerOW3.TIM_Period = 1000;
	timerOW3.TIM_ClockDivision = TIM_CKD_DIV1;
	TIM_TimeBaseInit(TIM2, &timerOW3);
	TIM_Cmd(TIM2, ENABLE);
	TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);

	nvicOW3.NVIC_IRQChannel = TIM2_IRQn;
	nvicOW3.NVIC_IRQChannelPreemptionPriority = 0;
	nvicOW3.NVIC_IRQChannelSubPriority = 1;
	nvicOW3.NVIC_IRQChannelCmd = ENABLE;

	NVIC_Init(&nvicOW3);
}




uint8_t onewire_OW3_sendResetBasic(void){
	uint8_t presence;

	//480us drive low
	onewire_OW3Write(onewire_high);
	delayus(480);

	//Release line
	onewire_OW3Write(onewire_low);
	delayus(60);

	//Do Read - 47 us to perform read and switch to output, 22.4us to just read
	gpioOW3.GPIO_Mode = GPIO_Mode_IN;
	GPIO_Init(GPIOC, &gpioOW3);
	//do read
	presence = GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_6);

	//reset to output
	gpioOW3.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_Init(GPIOC, &gpioOW3);

	//wait 120us more
	delayus(180 - 61);

	//Done
	return presence;
}

uint8_t onewire_OW3_sendReset(void){
	uint8_t presence;

	//simple pattern
	onewire_OW3Write(onewire_low);
	onewire_OW3Write(onewire_high);
	onewire_OW3Write(onewire_low);
	onewire_OW3Write(onewire_high);
	onewire_OW3Write(onewire_low);
	onewire_OW3Write(onewire_high);
	onewire_OW3Write(onewire_low);
	onewire_OW3Write(onewire_high);

	//480us drive low
	timerOW3.TIM_Period = 480-1;

	onewire_OW3.state = onewire_reset;
	onewire_OW3.next_delay = 120;
	onewire_OW3.next_level = onewire_high;

	onewire_OW3Write(onewire_low);
	TIM_TimeBaseInit(TIM2, &timerOW3);
	TIM_Cmd(TIM2, ENABLE);
	TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);

	while(onewire_OW3.state != onewire_done){}; //interrupt will take care of releasing bus and setting up timer for next action

	//now perform a read in middle of slave response time
	gpioOW3.GPIO_Mode = GPIO_Mode_IN;
	GPIO_Init(GPIOC, &gpioOW3);
	//do read
	presence = GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_13);
	gpioOW3.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_Init(GPIOC, &gpioOW3);

	//setup timer for remaining cycle
	timerOW3.TIM_Period = 120-1;
	onewire_OW3.next_delay = 0; //will transistion to done state
	onewire_OW3.state = onewire_single;

	TIM_TimeBaseInit(TIM2, &timerOW3);
	TIM_Cmd(TIM2, ENABLE);
	TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);


	while(onewire_OW3.state != onewire_done){}; //interrupt will take care of finishing

	//simple pattern
	onewire_OW3Write(onewire_low);
	onewire_OW3Write(onewire_high);
	onewire_OW3Write(onewire_low);
	onewire_OW3Write(onewire_high);
	return presence;
}


