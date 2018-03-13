/*
 uart.cpp - esp8266 UART HAL

 Copyright (c) 2014 Ivan Grokhotkov. All rights reserved.
 This file is part of the esp8266 core for Arduino environment.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

 */


/**
 *  UART GPIOs
 *
 * UART0 TX: 1 or 2
 * UART0 RX: 3
 *
 * UART0 SWAP TX: 15
 * UART0 SWAP RX: 13
 *
 *
 * UART1 TX: 7 (NC) or 2
 * UART1 RX: 8 (NC)
 *
 * UART1 SWAP TX: 11 (NC)
 * UART1 SWAP RX: 6 (NC)
 *
 * NC = Not Connected to Module Pads --> No Access
 *
 */
#include "Arduino.h"
#include "uart.h"
#include "esp8266_peri.h"
#include "user_interface.h"

static int s_uart_debug_nr = UART0;

struct uart_rx_buffer_ {
    size_t size;
    size_t rpos;
    size_t wpos;
    uint8_t * buffer;
};

struct uart_ {
    int uart_nr;
    int baud_rate;
    bool rx_enabled;
    bool tx_enabled;
    uint8_t rx_pin;
    uint8_t tx_pin;
    struct uart_rx_buffer_ * rx_buffer;
};

size_t uart_resize_rx_buffer(uart_t* uart, size_t new_size)
{
    if(uart == NULL || !uart->rx_enabled) {
        return 0;
    }
    if(uart->rx_buffer->size == new_size) {
        return uart->rx_buffer->size;
    }
    uint8_t * new_buf = (uint8_t*)malloc(new_size);
    if(!new_buf) {
        return uart->rx_buffer->size;
    }
    size_t new_wpos = 0;
    ETS_UART_INTR_DISABLE();
    while(uart_rx_available(uart) && new_wpos < new_size) {
        new_buf[new_wpos++] = uart_read_char(uart);
    }
    uint8_t * old_buf = uart->rx_buffer->buffer;
    uart->rx_buffer->rpos = 0;
    uart->rx_buffer->wpos = new_wpos;
    uart->rx_buffer->size = new_size;
    uart->rx_buffer->buffer = new_buf;
    free(old_buf);
    ETS_UART_INTR_ENABLE();
    return uart->rx_buffer->size;
}

inline size_t uart_rx_buffer_available(uart_t* uart) {
    if(uart->rx_buffer->wpos < uart->rx_buffer->rpos) {
      return (uart->rx_buffer->wpos + uart->rx_buffer->size) - uart->rx_buffer->rpos;
    }
    return uart->rx_buffer->wpos - uart->rx_buffer->rpos;
}

inline size_t uart_rx_fifo_available(uart_t* uart) {
    return (USS(uart->uart_nr) >> USRXC) & 0x7F;
}

// Copy all the rx fifo bytes that fit into the rx buffer
inline void uart_rx_copy_fifo_to_buffer(uart_t* uart) {
    while(uart_rx_fifo_available(uart)){
        size_t nextPos = (uart->rx_buffer->wpos + 1) % uart->rx_buffer->size;
        if(nextPos == uart->rx_buffer->rpos) {
            // Stop copying if rx buffer is full
            break;
        }
        uint8_t data = USF(uart->uart_nr);
        uart->rx_buffer->buffer[uart->rx_buffer->wpos] = data;
        uart->rx_buffer->wpos = nextPos;
    }
}

int uart_peek_char(uart_t* uart)
{
    if(uart == NULL || !uart->rx_enabled) {
        return -1;
    }
    if (!uart_rx_available(uart)) {
        return -1;
    }
    if (uart_rx_buffer_available(uart) == 0) {
        ETS_UART_INTR_DISABLE();
        uart_rx_copy_fifo_to_buffer(uart);
        ETS_UART_INTR_ENABLE();
    }
    return uart->rx_buffer->buffer[uart->rx_buffer->rpos];
}

int uart_read_char(uart_t* uart)
{
    if(uart == NULL) {
        return -1;
    }
    int data = uart_peek_char(uart);
    if(data != -1) {
        uart->rx_buffer->rpos = (uart->rx_buffer->rpos + 1) % uart->rx_buffer->size;
    }
    return data;
}

size_t uart_rx_available(uart_t* uart)
{
    if(uart == NULL || !uart->rx_enabled) {
        return 0;
    }
    return uart_rx_buffer_available(uart) + uart_rx_fifo_available(uart);
}

static void ICACHE_RAM_ATTR uart_isr_handle_data(void* arg, uint8_t data)
{
    uart_t* uart = (uart_t*)arg;
    if(uart == NULL || !uart->rx_enabled) {
        return;
    }
    size_t nextPos = (uart->rx_buffer->wpos + 1) % uart->rx_buffer->size;
    if(nextPos != uart->rx_buffer->rpos) {
        uart->rx_buffer->buffer[uart->rx_buffer->wpos] = data;
        uart->rx_buffer->wpos = nextPos;
    }
}

static void ICACHE_RAM_ATTR uart_isr(void* arg)
{
    uart_t* uart = (uart_t*)arg;
    if(uart == NULL || !uart->rx_enabled) {
        USIC(uart->uart_nr) = USIS(uart->uart_nr);
        ETS_UART_INTR_DISABLE();
        return;
    }
    if(USIS(uart->uart_nr) & ((1 << UIFF) | (1 << UITO))) {
        uart_rx_copy_fifo_to_buffer(uart);
    }
    USIC(uart->uart_nr) = USIS(uart->uart_nr);
}

static void uart_start_isr(uart_t* uart)
{
    if(uart == NULL || !uart->rx_enabled) {
        return;
    }
    if(gdbstub_has_uart_isr_control()) {
        gdbstub_set_uart_isr_callback(uart_isr_handle_data,  (void *)uart);
        return;
    }
    // UCFFT value is when the RX fifo full interrupt triggers.  A value of 1
    // triggers the IRS very often.  A value of 127 would not leave much time
    // for ISR to clear fifo before the next byte is dropped.  So pick a value
    // in the middle.
    USC1(uart->uart_nr) = (100 << UCFFT) | (0x02 << UCTOT) | (1 <<UCTOE);
    USIC(uart->uart_nr) = 0xffff;
    USIE(uart->uart_nr) = (1 << UIFF) | (1 << UIFR) | (1 << UITO);
    ETS_UART_INTR_ATTACH(uart_isr,  (void *)uart);
    ETS_UART_INTR_ENABLE();
}

static void uart_stop_isr(uart_t* uart)
{
    if(uart == NULL || !uart->rx_enabled) {
        return;
    }
    if(gdbstub_has_uart_isr_control()) {
        gdbstub_set_uart_isr_callback(NULL, NULL);
        return;
    }
    ETS_UART_INTR_DISABLE();
    USC1(uart->uart_nr) = 0;
    USIC(uart->uart_nr) = 0xffff;
    USIE(uart->uart_nr) = 0;
    ETS_UART_INTR_ATTACH(NULL, NULL);
}

static void uart_do_write_char(int uart_nr, char c)
{
    while((USS(uart_nr) >> USTXC) >= 0x7f) ;
    USF(uart_nr) = c;
}

void uart_write_char(uart_t* uart, char c)
{
    if(uart == NULL || !uart->tx_enabled) {
        return;
    }
    if(gdbstub_has_uart_isr_control() && uart->uart_nr == UART0) {
        gdbstub_write_char(c);
        return;
    }
    uart_do_write_char(uart->uart_nr, c);
}

void uart_write(uart_t* uart, const char* buf, size_t size)
{
    if(uart == NULL || !uart->tx_enabled) {
        return;
    }
    if(gdbstub_has_uart_isr_control() && uart->uart_nr == UART0) {
        gdbstub_write(buf, size);
        return;
    }
    while(size--) {
        uart_do_write_char(uart->uart_nr, *buf++);
    }
}

size_t uart_tx_free(uart_t* uart)
{
    if(uart == NULL || !uart->tx_enabled) {
        return 0;
    }
    return UART_TX_FIFO_SIZE - ((USS(uart->uart_nr) >> USTXC) & 0xff);
}

void uart_wait_tx_empty(uart_t* uart)
{
    if(uart == NULL || !uart->tx_enabled) {
        return;
    }
    while(((USS(uart->uart_nr) >> USTXC) & 0xff) > 0) {
        delay(0);
    }
}

void uart_flush(uart_t* uart)
{
    if(uart == NULL) {
        return;
    }

    uint32_t tmp = 0x00000000;
    if(uart->rx_enabled) {
        tmp |= (1 << UCRXRST);
        ETS_UART_INTR_DISABLE();
        uart->rx_buffer->rpos = 0;
        uart->rx_buffer->wpos = 0;
        ETS_UART_INTR_ENABLE();
    }

    if(uart->tx_enabled) {
        tmp |= (1 << UCTXRST);
    }

    if(!gdbstub_has_uart_isr_control() || uart->uart_nr != UART0) {
        USC0(uart->uart_nr) |= (tmp);
        USC0(uart->uart_nr) &= ~(tmp);
    }
}

void uart_set_baudrate(uart_t* uart, int baud_rate)
{
    if(uart == NULL) {
        return;
    }
    uart->baud_rate = baud_rate;
    USD(uart->uart_nr) = (ESP8266_CLOCK / uart->baud_rate);
}

int uart_get_baudrate(uart_t* uart)
{
    if(uart == NULL) {
        return 0;
    }
    return uart->baud_rate;
}

static void uart0_enable_tx_pin(uint8_t pin)
{
    switch(pin) {
    case 1:
        pinMode(pin, SPECIAL);
        break;
    case 2:
    case 15:
        pinMode(pin, FUNCTION_4);
        break;
    }
}

static void uart0_enable_rx_pin(uint8_t pin)
{
    switch(pin) {
        case 3:
            pinMode(pin, SPECIAL);
            break;
        case 13:
            pinMode(pin, FUNCTION_4);
            break;
    }
}

static void uart1_enable_tx_pin(uint8_t pin)
{
    if(pin == 2) {
        pinMode(pin, SPECIAL);
    }
}

static void uart_disable_pin(uint8_t pin)
{
    pinMode(pin, INPUT);
}

uart_t* uart_init(int uart_nr, int baudrate, int config, int mode, int tx_pin, size_t rx_size)
{
    uart_t* uart = (uart_t*) malloc(sizeof(uart_t));
    if(uart == NULL) {
        return NULL;
    }

    uart->uart_nr = uart_nr;

    switch(uart->uart_nr) {
    case UART0:
        ETS_UART_INTR_DISABLE();
        if(!gdbstub_has_uart_isr_control()) {
            ETS_UART_INTR_ATTACH(NULL, NULL);
        }
        uart->rx_enabled = (mode != UART_TX_ONLY);
        uart->tx_enabled = (mode != UART_RX_ONLY);
        uart->rx_pin = (uart->rx_enabled)?3:255;
        if(uart->rx_enabled) {
            struct uart_rx_buffer_ * rx_buffer = (struct uart_rx_buffer_ *)malloc(sizeof(struct uart_rx_buffer_));
            if(rx_buffer == NULL) {
              free(uart);
              return NULL;
            }
            rx_buffer->size = rx_size;//var this
            rx_buffer->rpos = 0;
            rx_buffer->wpos = 0;
            rx_buffer->buffer = (uint8_t *)malloc(rx_buffer->size);
            if(rx_buffer->buffer == NULL) {
              free(rx_buffer);
              free(uart);
              return NULL;
            }
            uart->rx_buffer = rx_buffer;
            uart0_enable_rx_pin(uart->rx_pin);
        }
        if(uart->tx_enabled) {
            if (tx_pin == 2) {
                uart->tx_pin = 2;
            } else {
                uart->tx_pin = 1;
            }
            uart0_enable_tx_pin(uart->tx_pin);
        } else {
            uart->tx_pin = 255;
        }
        IOSWAP &= ~(1 << IOSWAPU0);
        break;
    case UART1:
        // Note: uart_interrupt_handler does not support RX on UART 1.
        uart->rx_enabled = false;
        uart->tx_enabled = (mode != UART_RX_ONLY);
        uart->rx_pin = 255;
        uart->tx_pin = (uart->tx_enabled)?2:255;  // GPIO7 as TX not possible! See GPIO pins used by UART
        if(uart->tx_enabled) {
            uart1_enable_tx_pin(uart->tx_pin);
        }
        break;
    case UART_NO:
    default:
        // big fail!
        free(uart);
        return NULL;
    }

    uart_set_baudrate(uart, baudrate);
    USC0(uart->uart_nr) = config;
    if(!gdbstub_has_uart_isr_control() || uart->uart_nr != UART0) {
        uart_flush(uart);
        USC1(uart->uart_nr) = 0;
        USIC(uart->uart_nr) = 0xffff;
        USIE(uart->uart_nr) = 0;
    }
    if(uart->uart_nr == UART0) {
        if(uart->rx_enabled && !gdbstub_has_uart_isr_control()) {
            uart_start_isr(uart);
        }
        if(gdbstub_has_uart_isr_control()) {
            ETS_UART_INTR_ENABLE();
        }
    }

    return uart;
}

void uart_uninit(uart_t* uart)
{
    if(uart == NULL) {
        return;
    }

    if(uart->tx_enabled && (!gdbstub_has_uart_isr_control() || uart->uart_nr != UART0)) {
        uart_disable_pin(uart->tx_pin);
    }

    if(uart->rx_enabled) {
        free(uart->rx_buffer->buffer);
        free(uart->rx_buffer);
        if(!gdbstub_has_uart_isr_control()) {
            uart_disable_pin(uart->rx_pin);
            uart_stop_isr(uart);
        }
    }
    free(uart);
}

void uart_swap(uart_t* uart, int tx_pin)
{
    if(uart == NULL) {
        return;
    }
    switch(uart->uart_nr) {
    case UART0:
        if(uart->tx_enabled) { //TX
            uart_disable_pin(uart->tx_pin);
        }
        if(uart->rx_enabled) { //RX
            uart_disable_pin(uart->rx_pin);
        }

        if(((uart->tx_pin == 1 || uart->tx_pin == 2) && uart->tx_enabled)
                || (uart->rx_pin == 3 && uart->rx_enabled)) {
            if(uart->tx_enabled) { //TX
                uart->tx_pin = 15;
            }
            if(uart->rx_enabled) { //RX
                uart->rx_pin = 13;
            }
            IOSWAP |= (1 << IOSWAPU0);
        } else {
            if(uart->tx_enabled) { //TX
                uart->tx_pin = (tx_pin == 2)?2:1;
            }
            if(uart->rx_enabled) { //RX
                uart->rx_pin = 3;
            }
            IOSWAP &= ~(1 << IOSWAPU0);
        }

        if(uart->tx_enabled) { //TX
            uart0_enable_tx_pin(uart->tx_pin);
        }
        if(uart->rx_enabled) { //RX
            uart0_enable_rx_pin(uart->rx_pin);
        }

        break;
    case UART1:
        // Currently no swap possible! See GPIO pins used by UART
        break;
    default:
        break;
    }
}

void uart_set_tx(uart_t* uart, int tx_pin)
{
    if(uart == NULL) {
        return;
    }
    switch(uart->uart_nr) {
    case UART0:
        if(uart->tx_enabled) {
            if (uart->tx_pin == 1 && tx_pin == 2) {
                uart_disable_pin(uart->tx_pin);
                uart->tx_pin = 2;
                uart0_enable_tx_pin(uart->tx_pin);
            } else if (uart->tx_pin == 2 && tx_pin != 2) {
                uart_disable_pin(uart->tx_pin);
                uart->tx_pin = 1;
                uart0_enable_tx_pin(uart->tx_pin);
            }
        }

        break;
    case UART1:
        // GPIO7 as TX not possible! See GPIO pins used by UART
        break;
    default:
        break;
    }
}

void uart_set_pins(uart_t* uart, int tx, int rx)
{
    if(uart == NULL) {
        return;
    }

    if(uart->uart_nr == UART0) { // Only UART0 allows pin changes
        if(uart->tx_enabled && uart->tx_pin != tx) {
            if(rx == 13 && tx == 15) {
                uart_swap(uart, 15);
            } else if (rx == 3 && (tx == 1 || tx == 2)) {
                if (uart->rx_pin != rx) {
                    uart_swap(uart, tx);
                } else {
                    uart_set_tx(uart, tx);
                }
            }
        }
        if(uart->rx_enabled && uart->rx_pin != rx && rx == 13 && tx == 15) {
            uart_swap(uart, 15);
        }
    }
}


bool uart_tx_enabled(uart_t* uart)
{
    if(uart == NULL) {
        return false;
    }
    return uart->tx_enabled;
}

bool uart_rx_enabled(uart_t* uart)
{
    if(uart == NULL) {
        return false;
    }
    return uart->rx_enabled;
}


static void uart_ignore_char(char c)
{
    (void) c;
}

static void uart0_write_char(char c)
{
    while(((USS(0) >> USTXC) & 0xff) >= 0x7F) {
        delay(0);
    }
    USF(0) = c;
}

static void uart1_write_char(char c)
{
    while(((USS(1) >> USTXC) & 0xff) >= 0x7F) {
        delay(0);
    }
    USF(1) = c;
}

void uart_set_debug(int uart_nr)
{
    s_uart_debug_nr = uart_nr;
    void (*func)(char) = NULL;
    switch(s_uart_debug_nr) {
    case UART0:
        func = &uart0_write_char;
        break;
    case UART1:
        func = &uart1_write_char;
        break;
    case UART_NO:
    default:
        func = &uart_ignore_char;
        break;
    }
    if(!gdbstub_has_putc1_control()) {
        system_set_os_print((uint8)((uart_nr == UART0 || uart_nr == UART1)?1:0));
        ets_install_putc1((void *) func);
    } else {
        gdbstub_set_putc1_callback(func);
    }
}

int uart_get_debug()
{
    return s_uart_debug_nr;
}

void gdbstub_hook_enable_tx_pin_uart0(uint8_t pin) __attribute__((alias("uart0_enable_tx_pin")));
void gdbstub_hook_enable_rx_pin_uart0(uint8_t pin) __attribute__((alias("uart0_enable_rx_pin")));
