# 05_HAL_Part_2_Human_Interface_Displays_and_Power.md

## 1. Display Controllers (The Visual Cortex)
Rockbox supports a bewildering array of display technologies: Mono OLED, Color LCD, Greyscale, Parallel 8/16-bit, SPI, and MIPI (on newer targets).
The driver interface is standardized (`lcd-*.c`), allowing the UI engine to draw pixels without caring about the underlying bus.

### 1.1 The LCD Driver Interface
All display drivers must implement a core set of functions:
*   `lcd_init()`: Power on sequence.
*   `lcd_update()`: Push the framebuffer to the display.
*   `lcd_set_contrast(val)`: Adjust VCOM voltage.
*   `lcd_set_drawmode(mode)`: XOR, OR, SET, CLEAR.

**Source Analysis: `firmware/drivers/lcd-remote-1bit-v.c` (1-bit Vertical Packing)**
This driver handles simple monochrome displays where 8 pixels are packed vertically into a single byte.

```c
/* Update the display.
 * This function copies the framebuffer from SDRAM to the LCD controller's RAM.
 */
void lcd_update(void)
{
    /* 1. Set Column Address Range (0 - 127) */
    lcd_comm(0x21);
    lcd_data(0);
    lcd_data(127);

    /* 2. Set Page Address Range (0 - 7) */
    lcd_comm(0x22);
    lcd_data(0);
    lcd_data(7);

    /* 3. Write Framebuffer Data */
    /* Often uses DMA here for speed */
    for (int page = 0; page < 8; page++) {
        for (int col = 0; col < 128; col++) {
             lcd_data(framebuffer[page][col]);
        }
    }
}
```

### 1.2 Initialization Sequence (The "Magic Hex")
LCD controllers require precise, timed sequences of commands to initialize charge pumps, set bias ratios, and turn on the display. These are often reverse-engineered from original firmware dumps.

```c
/* Example SSD1306 Initialization */
static const unsigned char lcd_init_cmd[] = {
    0xAE,       /* Display OFF */
    0xD5, 0x80, /* Set Display Clock Divide Ratio */
    0xA8, 0x3F, /* Set Multiplex Ratio */
    0xD3, 0x00, /* Set Display Offset */
    0x40,       /* Set Display Start Line */
    0x8D, 0x14, /* Charge Pump Setting */
    0xAF        /* Display ON */
};
```

---

## 2. Input Matrices (`firmware/drivers/button.c`)
Rockbox handles input via GPIO scanning (Matrix Keypad) or specialized controllers (iPod Clickwheel).

### 2.1 The Button Matrix Scanner
Physical buttons are wired in a row/column matrix to save GPIO pins.
*   **Scanning Logic:**
    1.  Drive Row 1 LOW.
    2.  Read Columns. If any column is LOW, a key is pressed.
    3.  Repeat for all Rows.
    4.  Debounce (wait for stability).

**Source Analysis: `firmware/drivers/button.c`**
```c
static int button_read(int *data)
{
    int btn = 0;

    /* Scan Matrix */
    GPIO_OUT &= ~ROW_MASK; /* Drive rows low */

    if (!(GPIO_IN & COL_1)) btn |= BUTTON_LEFT;
    if (!(GPIO_IN & COL_2)) btn |= BUTTON_RIGHT;

    /* Handle Debounce */
    if (btn == lastbtn) {
        if (current_tick - last_change > DEBOUNCE_TIME) {
             return btn; /* Stable press */
        }
    }
    lastbtn = btn;
    return BUTTON_NONE;
}
```

### 2.2 The Message Queue
The driver posts raw button events (e.g., `BUTTON_LEFT | BUTTON_REL`) to the kernel's `button_queue`.
The main thread (`action.c`) reads this queue and translates raw hardware codes into logical actions (`ACTION_WPS_PLAY`).

---

## 3. Power Management (PMIC) & ADC
Rockbox is obsessed with battery life. It aggressively manages power states.

### 3.1 Voltage Monitoring (ADC)
The battery voltage is read via an Analog-to-Digital Converter (ADC).
*   **Calibration:** Raw ADC values (0-1023) are converted to millivolts using a linear scale factor derived from the schematic (voltage divider resistors).

### 3.2 Charging Logic (`firmware/powermgmt.c`)
Rockbox implements a software-controlled charging state machine.
*   **States:** `DISCHARGING`, `CHARGING`, `CHARGED`.
*   **Logic:**
    1.  Detect USB insertion (VBUS interrupt).
    2.  Enable charging IC (e.g., LTC4054).
    3.  Monitor current/voltage.
    4.  Stop charging when current drops below threshold (end-of-charge).

**Source Analysis: `firmware/powermgmt.c`**
```c
void power_thread(void)
{
    while (1) {
        int voltage = adc_read(ADC_BATTERY);

        /* Update Battery Level Icon */
        battery_level_update(voltage);

        /* Handle Charging State */
        if (usb_inserted()) {
            if (voltage < BATTERY_FULL_VOLTAGE) {
                 charger_enable(true);
            } else {
                 charger_enable(false);
            }
        }

        /* Auto Power Off */
        if (idle_time > global_settings.poweroff) {
             sys_poweroff();
        }

        sleep(HZ); /* Check once per second */
    }
}
```
*Note: This thread runs at a very low priority (`PRIORITY_SYSTEM`).*

### 3.3 Deep Sleep
When the device is "off" (soft off), the CPU enters a deep sleep mode, waking only on specific interrupts (Power Button, RTC Alarm, USB insertion). Rockbox configures the wakeup sources before executing the `WFI` (Wait For Interrupt) instruction.
