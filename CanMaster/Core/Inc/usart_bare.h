/*****************************************************************************************************************************
 * usart_bare.h - register-level USART1 (PA9 TX / PA10 RX, 115200 8N1).
 *
 * RX runs on the RXNE interrupt into a ring buffer: the F103 USART has no
 * FIFO and the main loop can sit in a CAN wait for seconds (activate), so a
 * polled RX would overrun the instant the host streams a 75-byte chunk.
 *****************************************************************************************************************************/

#ifndef USART_BARE_H_
#define USART_BARE_H_

#include <stdint.h>

void    usart_bare_init(void);
void    usart_bare_irq(void);                       /* call from USART1_IRQHandler */
uint8_t usart_bare_pop(uint8_t *b);                 /* 1 = one byte taken from ring */
void    usart_bare_write(const uint8_t *s, uint16_t n);
uint16_t usart_bare_pending(void);

#endif /* USART_BARE_H_ */
