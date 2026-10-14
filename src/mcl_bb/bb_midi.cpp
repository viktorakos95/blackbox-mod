/*
 * MIDI jack for MCL (milestone M3). USART6, 31250 baud, set up by the stock firmware (init 0x08044da0, one-byte HAL receive).
 *
 * RX   mcl_rx_tap replaces the one call to the stock ring push (0x080446d4 -> 0x080444f4, from 0x080446c4, from the RxCplt callback).
 *      It copies every byte of the jack (port block 0x24002e88) into MCL's own ring, then calls the stock push unchanged. Interrupt context,
 *      priority 0, tens of cycles. Independent of the clkmon hook (that one sits on the callback's call to 0x080446c4).
 * TX   mcl_tx_hook replaces the entry of the stock send function (0x08044dfc: port, buf, len; stock flush calls it once per message
 *      with the queue bytes). As long as MCL has not claimed the port it jumps to the stock code. Once claimed (HAL idle) it copies the
 *      message into MCL's ring and returns 1, so the stock firmware's own output (notes, clock) goes through the same single writer.
 *      The ring is drained by the USART6 TXE interrupt: mcl_usart6_isr wraps vector 71; it sends one byte per TXE, real-time bytes first
 *      (they may sit between the bytes of a message), and passes everything else (RX, errors) to the stock handler 0x080462e4 unchanged.
 *      CR1.TXEIE is only ever set under PRIMASK, so a task can't overwrite a CR1 change the HAL makes in the same interrupt.
 * All state is in this blob's SDRAM region; every hook is a no-op until bb_midi_init() has set the ready magic (set last, cleared first).
 */
#include "bb_midi.h"

#define USART6   0x40011400u
#define U_CR1    (*(volatile uint32_t *)(USART6 + 0x00))
#define U_ISR    (*(volatile uint32_t *)(USART6 + 0x1c))
#define U_TDR    (*(volatile uint32_t *)(USART6 + 0x28))
#define CR1_TXEIE 0x80u
#define ISR_TXE   0x80u
#define ISR_PASS  0x83fu                       /* PE FE NE ORE IDLE RXNE RTOF: anything the stock handler must see */
#define STOCK_MGR 0x24002e88u
#define HAL_HANDLE 0x24009914u                 /* port 0 UART_HandleTypeDef; gState at +0x80, READY = 0x20 */
#define HAL_GSTATE (*(volatile uint32_t *)(HAL_HANDLE + 0x80))
#define STOCK_PUSH  ((void (*)(void *, uint32_t))0x080444f5u)
#define STOCK_SEND  ((uint32_t (*)(uint32_t, const uint8_t *, uint32_t))0x08044e05u)   /* the send function after its port check */
#define STOCK_IRQ   ((void (*)(void))0x080462e5u)

#define RX_SIZE 4096u
#define TX_SIZE 8192u
#define READY_MAGIC 0x4d494431u                /* "MID1" */

static volatile uint32_t ready;
static volatile uint32_t owner, want, filter_clock;
static uint8_t rx_ring[RX_SIZE], tx_ring[TX_SIZE];
static volatile uint32_t rx_wr, rx_rd, tx_wr, tx_rd;
static volatile uint8_t rt[16];
static volatile uint32_t rt_wr, rt_rd;
static volatile uint32_t st_rx, st_rx_drop, st_tx, st_tx_rej, st_redirect, st_rt_rej;

/* entered from patched stock code, or the API MCL calls: the linker must keep them */
#define HOOK __attribute__((section(".hook"), used))

static inline uint32_t irq_off(void)
{
    uint32_t p;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(p)::"memory");
    return p;
}
static inline void irq_restore(uint32_t p) { __asm volatile("msr primask, %0" ::"r"(p) : "memory"); }

extern "C" {

HOOK void bb_midi_down(void) { ready = 0; }

HOOK void bb_midi_init(void)
{
    rx_wr = rx_rd = tx_wr = tx_rd = rt_wr = rt_rd = 0;
    owner = want = filter_clock = 0;
    st_rx = st_rx_drop = st_tx = st_tx_rej = st_redirect = st_rt_rej = 0;
    ready = READY_MAGIC;
}

HOOK void mcl_rx_tap(void *block, uint32_t byte)
{
    if (ready == READY_MAGIC && (uint32_t)block == STOCK_MGR) {
        uint32_t w = rx_wr;
        if (w - rx_rd < RX_SIZE) {
            rx_ring[w & (RX_SIZE - 1)] = (uint8_t)byte;
            rx_wr = w + 1;
            st_rx++;
        } else {
            st_rx_drop++;
        }
    }
    STOCK_PUSH(block, byte);
}

HOOK int bb_midi_rx_get(void)
{
    uint32_t r = rx_rd;
    if (r == rx_wr)
        return -1;
    uint8_t b = rx_ring[r & (RX_SIZE - 1)];
    rx_rd = r + 1;
    return b;
}

HOOK uint32_t bb_midi_tx_free(void) { return TX_SIZE - (tx_wr - tx_rd); }

HOOK uint32_t bb_midi_tx(const uint8_t *p, uint32_t n)
{
    if (ready != READY_MAGIC || !n)
        return 0;
    uint32_t i = irq_off();                    /* several writers (MCL tasks, the stock flush): one at a time */
    uint32_t w = tx_wr;
    if (TX_SIZE - (w - tx_rd) < n) {
        st_tx_rej++;
        irq_restore(i);
        return 0;
    }
    for (uint32_t k = 0; k < n; k++)
        tx_ring[(w + k) & (TX_SIZE - 1)] = p[k];
    tx_wr = w + n;
    if (owner)
        U_CR1 |= CR1_TXEIE;
    irq_restore(i);
    return n;
}

HOOK int bb_midi_tx_rt(uint8_t b)
{
    if (ready != READY_MAGIC)
        return 0;
    uint32_t i = irq_off();
    int ok = rt_wr - rt_rd < 16;
    if (ok) {
        rt[rt_wr & 15] = b;
        rt_wr++;
        if (owner)
            U_CR1 |= CR1_TXEIE;
    } else {
        st_rt_rej++;
    }
    irq_restore(i);
    return ok;
}

HOOK void bb_midi_claim(int immediate)
{
    if (immediate) {
        uint32_t i = irq_off();
        if (HAL_GSTATE == 0x20u)
            owner = 1;
        irq_restore(i);
    } else {
        want = 1;                              /* taken by the next stock send, when the HAL is idle */
    }
}

HOOK int bb_midi_owner(void) { return (int)owner; }

HOOK void bb_midi_filter_clock(int on) { filter_clock = on ? 1u : 0u; }

HOOK uint32_t bb_midi_stat(int which)
{
    switch (which) {
    case 0: return st_rx;
    case 1: return st_rx_drop;
    case 2: return st_tx;
    case 3: return st_tx_rej;
    case 5: return st_rt_rej;
    default: return st_redirect;
    }
}

HOOK uint32_t mcl_tx_hook(uint32_t port, const uint8_t *buf, uint32_t len)
{
    if (port > 1)
        return 0;                              /* the stock function's own first check */
    if (ready == READY_MAGIC && port == 0) {
        if (want && !owner) {
            uint32_t i = irq_off();
            if (HAL_GSTATE == 0x20u)
                owner = 1;
            irq_restore(i);
        }
        if (owner) {
            if (filter_clock && len == 1 && buf[0] >= 0xf8 && buf[0] <= 0xfc) {
                st_redirect++;                  /* the stock clock / transport byte: dropped, MCL's own timer is the source */
                return 1;
            }
            if (!bb_midi_tx(buf, len))
                return 0;                      /* ring full: the stock side sees a busy port and tries again */
            st_redirect++;
            return 1;
        }
    }
    return STOCK_SEND(port, buf, len);
}

HOOK void mcl_usart6_isr(void)
{
    if (ready == READY_MAGIC && owner) {
        uint32_t isr = U_ISR;
        if ((U_CR1 & CR1_TXEIE) && (isr & ISR_TXE)) {
            if (rt_rd != rt_wr) {
                U_TDR = rt[rt_rd & 15];
                rt_rd++;
                st_tx++;
            } else if (tx_rd != tx_wr) {
                U_TDR = tx_ring[tx_rd & (TX_SIZE - 1)];
                tx_rd++;
                st_tx++;
            } else {
                U_CR1 &= ~CR1_TXEIE;
            }
        }
        if (!(isr & ISR_PASS))
            return;
    }
    STOCK_IRQ();
}

}
