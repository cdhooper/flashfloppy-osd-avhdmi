/*
 * stm32f10x.c
 * 
 * Core and peripheral registers.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

struct extra_exception_frame {
    uint32_t r4, r5, r6, r7, r8, r9, r10, r11, lr;
};

void EXC_unexpected(struct extra_exception_frame *extra)
{
    struct exception_frame *frame;
    uint8_t exc = (uint8_t)read_special(psr);
    uint32_t msp, psp;

    if (extra->lr & 4) {
        frame = (struct exception_frame *)read_special(psp);
        psp = (uint32_t)(frame + 1);
        msp = (uint32_t)(extra + 1);
    } else {
        frame = (struct exception_frame *)(extra + 1);
        psp = read_special(psp);
        msp = (uint32_t)(frame + 1);
    }

    printk("Unexpected %s #%u at PC=%08x (%s):\n",
           (exc < 16) ? "Exception" : "IRQ",
           (exc < 16) ? exc : exc - 16,
           frame->pc, (extra->lr & 8) ? "Thread" : "Handler");
    printk(" r0:  %08x   r1:  %08x   r2:  %08x   r3:  %08x\n",
           frame->r0, frame->r1, frame->r2, frame->r3);
    printk(" r4:  %08x   r5:  %08x   r6:  %08x   r7:  %08x\n",
           extra->r4, extra->r5, extra->r6, extra->r7);
    printk(" r8:  %08x   r9:  %08x   r10: %08x   r11: %08x\n",
           extra->r8, extra->r9, extra->r10, extra->r11);
    printk(" r12: %08x   sp:  %08x   lr:  %08x   pc:  %08x\n",
           frame->r12, (extra->lr & 4) ? psp : msp, frame->lr, frame->pc);
    printk(" msp: %08x   psp: %08x   psr: %08x\n",
           msp, psp, frame->psr);

    system_reset();
}

static void exception_init(void)
{
    /* Initialise and switch to Process SP. Explicit asm as must be
     * atomic wrt updates to SP. We can't guarantee that in C. */
    asm volatile (
        "    mrs  r1,msp     \n"
        "    msr  psp,r1     \n" /* Set up Process SP    */
        "    movs r1,%0      \n"
        "    msr  control,r1 \n" /* Switch to Process SP */
        "    isb             \n" /* Flush the pipeline   */
        :: "i" (CONTROL_SPSEL) : "r1" );

    /* Set up Main SP for IRQ/Exception context. */
    write_special(msp, _irq_stacktop);

    /* Initialise interrupts and exceptions. */
    scb->vtor = (uint32_t)(unsigned long)vector_table;
    scb->ccr |= SCB_CCR_STKALIGN | SCB_CCR_DIV_0_TRP;
    /* GCC inlines memcpy() using full-word load/store regardless of buffer
     * alignment. Hence it is unsafe to trap on unaligned accesses. */
    /*scb->ccr |= SCB_CCR_UNALIGN_TRP;*/
    scb->shcsr |= (SCB_SHCSR_USGFAULTENA |
                   SCB_SHCSR_BUSFAULTENA |
                   SCB_SHCSR_MEMFAULTENA);

    /* SVCall/PendSV exceptions have lowest priority. */
    scb->shpr2 = 0xff<<24;
    scb->shpr3 = 0xff<<16;
}

unsigned int sysclk = 8000000;
unsigned int hse_clock = 0;

static void watchdog_kick(void)
{
    /* Reload the Watchdog. */
    iwdg->kr = 0xaaaa;
}

static unsigned int measure_hse_clock(void)
{
    unsigned int timeout;
    unsigned int hse_clock;
    uint16_t capture_start;
    uint16_t capture_end;
    uint16_t capture_time;

    /* Start up the internal oscillator (HSI) and switch to it. */
    rcc->cr |= RCC_CR_HSION;
    while (!(rcc->cr & RCC_CR_HSIRDY))
        ;
    rcc->cfgr &= ~RCC_CFGR_SW_MASK;
    rcc->cfgr |= RCC_CFGR_SW_HSI;

    /* Set up TIM2 clocked by HSI for time measurement */
    rcc->apb1enr |= RCC_APB1ENR_TIM2EN;
    tim2->psc = 0;           // No prescaler, 8 MHz resolution
    tim2->arr = 0x0000FFFF;  // Auto-reload value
    tim2->cr1 |= TIM_CR1_CEN;

    /* Start up the external oscillator (HSE). */
    timeout = 100000;
    rcc->cr |= RCC_CR_HSEON;
    while (!(rcc->cr & RCC_CR_HSERDY)) {
        if (timeout-- == 0)
            return (0);  // No external clock
        cpu_relax();
    }

    /* Enable RTC to use HSE/128 */
    rcc->apb1enr |= (RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN);
    pwr->cr |= PWR_CR_DBP; // Enable write access to Backup domain

    rcc->bdcr &= ~RCC_BDCR_RTCSEL;
    rcc->bdcr |= RCC_BDCR_RTCSEL_HSE; // HSE/128 is the default for this mux
    rcc->bdcr |= RCC_BDCR_RTCEN;

    /* Wait for RTC register synchronization */
    rtc->crl &= ~RTC_CRL_RSF;
    while (!(rtc->crl & RTC_CRL_RSF))
        ;
    while (!(rtc->crl & RTC_CRL_RTOFF))
        ;

    /*
     * Configure RTC "seconds" tick by setting the prescaler
     * for a very fast rate (128 * 100 ticks), which at 8 MHz HSE
     * would be about 1.6 msec.
     */
    rtc->crl |= RTC_CRL_CNF;
    rtc->prll = 99 & 0xFFFF;
    rtc->prlh = (99 >> 16) & 0x000F;
    rtc->crl &= ~RTC_CRL_CNF;
    rtc->crl &= ~RTC_CRL_SECF; // Clear "second" flag
    while (!(rtc->crl & RTC_CRL_RTOFF))
        ;

    /* Wait for RTC "seconds" tick to occur */
    while (!(rtc->crl & RTC_CRL_SECF))
        watchdog_kick();
    rtc->crl &= ~RTC_CRL_SECF; // Clear "second" flag

    capture_start = tim2->cnt;

    /* Wait for RTC "seconds" tick to occur again */
    while (!(rtc->crl & RTC_CRL_SECF))
        watchdog_kick();
    capture_end = tim2->cnt;
    capture_time = capture_end - capture_start;

    /* Shut down RTC */
    rcc->bdcr &= ~RCC_BDCR_RTCEN;
    rcc->apb1enr &= ~(RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN);
    pwr->cr &= ~PWR_CR_DBP;

    /*
     * capture_time is the number of HSI ticks which occurred in the
     * (HSE / 128 / 100) interval.
     * HSE = HSI * 128 * 100 / capture_time
     */
    printk("diff=%x %x %x\n", capture_time, capture_start, capture_end);
    hse_clock = HSI_CLOCK * 128 / capture_time * 100;
    printk("HSE=%u Hz\n", hse_clock);
    return (hse_clock);
}

static void clock_init(void)
{
    /* Flash controller: reads require 2 wait states at 72MHz. */
    flash->acr = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY(2);

    hse_clock = measure_hse_clock();

    /* PLLs, scalers, muxes. */
    if (hse_clock > 7800000) {
        /* HSE is standard 8 MHz */
        rcc->cfgr = (RCC_CFGR_PLLMUL(9) |        /* PLL = 9*8MHz = 72MHz */
                     RCC_CFGR_PLLSRC_PREDIV1 |
                     RCC_CFGR_ADCPRE_DIV8 |
                     RCC_CFGR_PPRE1_DIV2);
        sysclk = hse_clock * 9;
    } else if (hse_clock > 5000000) {
        /* Clocked by Amiga 7.15909 MHz or 7.09379 MHz CDAC */
        rcc->cfgr = (RCC_CFGR_PLLMUL(8) |    /* PLL = 8*7.159MHz = 57.272MHz */
                     RCC_CFGR_PLLSRC_PREDIV1 |
                     RCC_CFGR_ADCPRE_DIV8 |
                     RCC_CFGR_PPRE1_DIV2);
        sysclk = hse_clock * 8;
    } else  if (hse_clock > 3000000) {
        /* Clocked by Amiga 3.5795 MHz clock */
        rcc->cfgr = (RCC_CFGR_PLLMUL(16) |  /* PLL = 16*3.5795MHz = 57.272MHz */
                     RCC_CFGR_PLLSRC_PREDIV1 |
                     RCC_CFGR_ADCPRE_DIV8 |
                     RCC_CFGR_PPRE1_DIV2);
        sysclk = hse_clock * 16;
    } else {
        /* Clocked internally (8 MHz / 2) */
        rcc->cfgr = (RCC_CFGR_PLLMUL(16) |  /* PLL = 16 * 4MHz = 64MHz */
                     RCC_CFGR_ADCPRE_DIV8 |
                     RCC_CFGR_PPRE1_DIV1);
        sysclk = HSI_CLOCK / 2 * 16;
    }

    /* Enable and stabilise the PLL. */
    rcc->cr |= RCC_CR_PLLON;
    while (!(rcc->cr & RCC_CR_PLLRDY))
        cpu_relax();

    /* Switch to the externally-driven PLL for system clock. */
    rcc->cfgr |= RCC_CFGR_SW_PLL;
    while ((rcc->cfgr & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL)
        cpu_relax();

    /* Internal oscillator no longer needed. */
    rcc->cr &= ~RCC_CR_HSION;

    /* Enable SysTick counter at 72/8=9MHz. */
    stk->load = STK_MASK;
    stk->ctrl = STK_CTRL_ENABLE;
}

static void gpio_init(GPIO gpio)
{
    /* All pins are in weak Pull-Up mode. */
    gpio->bsrr = 0xffff;
    gpio->crl = gpio->crh = 0x88888888u;
}

static void peripheral_init(void)
{
    /* Enable basic GPIO and AFIO clocks, DMA, all timers, and all SPI. */
    rcc->apb1enr = (RCC_APB1ENR_TIM2EN |
                    RCC_APB1ENR_TIM3EN |
                    RCC_APB1ENR_TIM4EN |
                    RCC_APB1ENR_SPI2EN);

    rcc->apb2enr = (RCC_APB2ENR_IOPAEN |
                    RCC_APB2ENR_IOPBEN |
                    RCC_APB2ENR_IOPCEN |
                    RCC_APB2ENR_AFIOEN |
                    RCC_APB2ENR_TIM1EN |
                    RCC_APB2ENR_SPI1EN);

    rcc->ahbenr = RCC_AHBENR_DMA1EN;

    /* Enable SWD, Turn off serial-wire JTAG and reclaim the GPIOs.
     * Amiga keyboard map PB4 (KBCLK) to TIM3,CH1 and use its input filter
     * and edge detector to generate interrupts on clock transitions. */
    afio->mapr = (AFIO_MAPR_SWJ_CFG_JTAGDISABLE
                 | AFIO_MAPR_TIM3_REMAP_PARTIAL);

    /* All pins in a stable state. */
    gpio_init(gpioa);
    gpio_init(gpiob);
    gpio_init(gpioc);
}

void stm32_init(void)
{
    exception_init();
    peripheral_init();
    console_init();
    cpu_sync();
#define AMIGA_AV_TO_HDMI  // For STM32 integrated on Amiga AV to HDMI board
#ifdef AMIGA_AV_TO_HDMI
    /* Wait a little longer to start */
    for (volatile int i = 0; i < 100000; i++)
        ;
    printk("\n// START //\n\n\n");
#endif
    clock_init();
    cpu_sync();
}

static void fpec_wait_and_clear(void)
{
    while (flash->sr & FLASH_SR_BSY)
        continue;
    flash->sr = FLASH_SR_EOP | FLASH_SR_WRPRTERR | FLASH_SR_PGERR;
    flash->cr = 0;
}

void fpec_init(void)
{
    /* Erases and writes require the HSI oscillator. */
    rcc->cr |= RCC_CR_HSION;
    while (!(rcc->cr & RCC_CR_HSIRDY))
        cpu_relax();

    /* Unlock the FPEC. */
    if (flash->cr & FLASH_CR_LOCK) {
        flash->keyr = 0x45670123;
        flash->keyr = 0xcdef89ab;
    }

    fpec_wait_and_clear();
}

void fpec_page_erase(uint32_t flash_address)
{
    fpec_wait_and_clear();
    flash->cr |= FLASH_CR_PER;
    flash->ar = flash_address;
    flash->cr |= FLASH_CR_STRT;
    fpec_wait_and_clear();
}

void fpec_write(const void *data, unsigned int size, uint32_t flash_address)
{
    uint16_t *_f = (uint16_t *)flash_address;
    const uint16_t *_d = data;

    fpec_wait_and_clear();
    for (; size != 0; size -= 2) {
        flash->cr |= FLASH_CR_PG;
        *_f++ = *_d++; 
        fpec_wait_and_clear();
   }
}

void delay_ticks(unsigned int ticks)
{
    unsigned int diff, cur, prev = stk->val;

    for (;;) {
        cur = stk->val;
        diff = (prev - cur) & STK_MASK;
        if (ticks <= diff)
            break;
        ticks -= diff;
        prev = cur;
    }
}

void delay_ns(unsigned int ns)
{
    delay_ticks((ns * STK_MHZ) / 1000u);
}

void delay_us(unsigned int us)
{
    delay_ticks(us * STK_MHZ);
}

void delay_ms(unsigned int ms)
{
    delay_ticks(ms * 1000u * STK_MHZ);
}

void gpio_configure_pin(GPIO gpio, unsigned int pin, unsigned int mode)
{
    gpio_write_pin(gpio, pin, mode >> 4);
    mode &= 0xfu;
    if (pin >= 8) {
        pin -= 8;
        gpio->crh = (gpio->crh & ~(0xfu<<(pin<<2))) | (mode<<(pin<<2));
    } else {
        gpio->crl = (gpio->crl & ~(0xfu<<(pin<<2))) | (mode<<(pin<<2));
    }
}

bool_t gpio_pins_connected(GPIO gpio1, unsigned int pin1,
                           GPIO gpio2, unsigned int pin2)
{
    bool_t connected = FALSE;

    /* STEP 1. Use only weak internal pullups and pulldowns, to determine
     * whether the pins are externally driven or tied. */

    /* Can both pins pull low? */
    gpio_configure_pin(gpio1, pin1, GPI_pull_down);
    gpio_configure_pin(gpio2, pin2, GPI_pull_down);
    delay_us(5);
    if (gpio_read_pin(gpio1, pin1) || gpio_read_pin(gpio2, pin2))
        goto out;

    /* Can both pins pull up? */
    gpio_configure_pin(gpio1, pin1, GPI_pull_up);
    gpio_configure_pin(gpio2, pin2, GPI_pull_up);
    delay_us(5);
    if (!gpio_read_pin(gpio1, pin1) || !gpio_read_pin(gpio2, pin2))
        goto out;

    /* STEP 2. Drive one pin and then the other LOW, to determine whether 
     * the pins are connected. */

    /* Can pin2 pull pin1 low? */
    gpio_configure_pin(gpio1, pin1, GPI_pull_up);
    gpio_configure_pin(gpio2, pin2, GPO_pushpull(_2MHz, LOW));
    delay_us(5);
    if (gpio_read_pin(gpio1, pin1))
        goto out;

    /* Can pin1 pull pin2 low? */
    gpio_configure_pin(gpio2, pin2, GPI_pull_up);
    gpio_configure_pin(gpio1, pin1, GPO_pushpull(_2MHz, LOW));
    delay_us(5);
    if (gpio_read_pin(gpio2, pin2))
        goto out;

    connected = TRUE;

out:
    /* Return pins to their default configuration. */
    gpio_configure_pin(gpio1, pin1, GPI_pull_up);
    gpio_configure_pin(gpio2, pin2, GPI_pull_up);
    return connected;
}

void system_reset(void)
{
    console_sync();
    printk("Resetting...\n");
    /* Wait for serial console TX to idle. */
    while (!(usart1->sr & USART_SR_TXE) || !(usart1->sr & USART_SR_TC))
        cpu_relax();
    /* Request reset and loop waiting for it to happen. */
    scb->aircr = SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESETREQ;
    for (;;) ;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
