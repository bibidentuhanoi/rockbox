# 05_HAL_Part_2_Human_Interface_Displays_and_Power.md

## 1. Display Controllers (The Visual Cortex)
Rockbox supports a bewildering array of display technologies: Mono OLED, Color LCD, Greyscale, Parallel 8/16-bit, SPI, and MIPI (on newer targets). The driver interface is standardized (`lcd-*.c`), allowing the UI engine to draw pixels without caring about the underlying bus.

### 1.1 The LCD Driver Interface (Level 4)
All display drivers must implement a core set of functions that abstract the hardware:
*   `lcd_init()`: Power on sequence.
*   `lcd_update()`: Push the framebuffer to the display.
*   `lcd_set_contrast(val)`: Adjust VCOM voltage.
*   `lcd_set_drawmode(mode)`: XOR, OR, SET, CLEAR.

**The Framebuffer Format:**
The logical framebuffer (`framebuffer[]`) matches the native pixel format of the display controller to minimize conversion overhead during updates.
*   **1-bit Vertical:** 8 pixels packed into one byte, vertical striping (SSD1306).
*   **16-bit RGB565:** Standard embedded color format (5 bits Red, 6 Green, 5 Blue).
*   **18-bit RGB666:** Unpacked to 3 bytes or packed into 4 bytes.

---

### 1.2 Deep Dive: AS3525 DBOP LCD Driver (Level 10 Depth)
The Sansa Clip (AS3525 SoC) uses a dedicated "Data Block Output Port" (DBOP) peripheral to drive its OLED via an 8-bit parallel bus. This section dissects the driver stack from the high-level API down to the physical wire signals.

#### Level 1: The User API Call
The UI thread decides to redraw the screen.
```c
/* apps/gui/screen_access.c */
screens[0].update(); /* Function pointer to lcd_update() */
```

#### Level 2: The Driver Update Logic
The driver iterates over the framebuffer rows/pages.
```c
/* firmware/target/arm/as3525/sansa-clip/lcd-clip.c */
void lcd_update(void) {
    for (int page = 0; page < 8; page++) {
        lcd_write_command(0xB0 + page); /* Set Page Address */
        lcd_write_command(0x00);        /* Set Column Lower Nibble */
        lcd_write_command(0x10);        /* Set Column Upper Nibble */
        lcd_write_data(&framebuffer[page][0], 128); /* Burst 128 bytes */
    }
}
```

#### Level 3: The Bus Abstraction
The `lcd_write_command` function abstracts the specific bus requirement of toggling the Data/Command pin.
```c
void lcd_write_command(int byte) {
    /* 1. Wait for Timing Delay */
    volatile int i = 0; while(i < LCD_DELAY) i++;

    /* 2. Assert Command Mode (D/C# Low) via GPIO */
    GPIOA_PIN(5) = 0;

    /* 3. Push to Hardware FIFO */
    DBOP_DOUT = (byte << 8) | byte;

    /* 4. Spinwait for FIFO Empty */
    while ((DBOP_STAT & (1<<10)) == 0);
}
```

#### Level 4: The Register Interface (`as3525.h`)
The macro `DBOP_DOUT` maps to a specific memory address in the APB (Advanced Peripheral Bus) bridge.
```c
/* firmware/export/as3525.h */
#define DBOP_BASE         0xC8120000
#define DBOP_DOUT         (*(volatile unsigned short*)(DBOP_BASE + 0x10))
#define DBOP_STAT         (*(volatile unsigned long *)(DBOP_BASE + 0x0C))
```

#### Level 5: Bit-Level Configuration (`DBOP_CTRL`)
The DBOP controller behavior is defined by the Control Register at `0xC8120008`.
*   **Initialization Value:** `0x51008`
*   **Bit Breakdown:**
    *   `Bit 0-2`: **000** (Word count per transfer, irrelevant for push mode)
    *   `Bit 3`: **1** (Enable Output)
    *   `Bit 12`: **1** (Push FIFO Mode enabled)
    *   `Bit 16`: **1** (Enable Clock Generation)
    *   `Bit 18`: **1** (Active High Polarity)

#### Level 6: The Clock Tree (`CGU_DBOP`)
The DBOP peripheral clock is derived from the main system PLL (Phase Locked Loop).
```c
/* firmware/target/arm/as3525/sansa-clip/lcd-clip.c */
CGU_DBOP = (1<<3) | AS3525_DBOP_DIV;
```
*   `Bit 3`: **CLOCK_ENABLE** (Gates the clock to the peripheral).
*   `Bits 0-2`: **DIV** (Divider ratio). If PLL is 192MHz and DIV=3 (ratio 8), DBOP clock = 24MHz.

#### Level 7: GPIO Routing (`AFSEL`)
The AS3525 uses "Alternate Function Select" registers to route internal peripherals to physical pads.
```c
GPIOB_AFSEL = 0x08; /* Pin B3 becomes DBOP_WR (Write Strobe) */
GPIOC_AFSEL = 0x0f; /* Pins C0-C3 become DBOP_D0-D3 */
```
This physically disconnects the GPIO controller from these pins and connects the DBOP logic.

#### Level 8: Timing Generators (`DBOP_TIMPOL`)
The parallel bus timing (Setup, Hold, Pulse Width) is programmable via `TIMPOL` registers to match the SSD1329 OLED controller specs.
*   **Register:** `DBOP_TIMPOL_01` (Address `0xC8120000`)
*   **Value:** `0x6E167`
    *   `Bits 0-7`: **0x67** (Pulse Width low time in clock cycles)
    *   `Bits 8-15`: **0xE1** (Pulse Width high time)
    *   `Bits 16-19`: **0x6** (Setup time)

#### Level 9: The Wire Protocol (8080 Parallel)
When `DBOP_DOUT = 0xAF` is executed, the hardware generates the following waveform on the physical traces:
1.  **CS# (Chip Select):** Driven LOW.
2.  **D/C# (Data/Command):** Driven LOW (set by software GPIO A5).
3.  **WR# (Write Strobe):** Driven LOW for `TIMPOL.LOW` cycles.
4.  **D0-D7 (Data Bus):** Driven with `0xAF` (10101111).
5.  **WR#:** Driven HIGH. Data latched by LCD on rising edge.
6.  **CS#:** Driven HIGH.

#### Level 10: The Physical Controller (SSD1329)
Inside the OLED panel, the SSD1329 controller receives `0xAF`:
1.  **Instruction Decoder:** Recognizes `0xAF` as "Display ON".
2.  **Power State Machine:** Activates the internal charge pump (VCC generation).
3.  **Gate Driver:** Begins scanning the OLED matrix rows.
4.  **Pixel Emission:** Organic LEDs light up based on GDRAM content.

---

## 2. Input Matrices (`firmware/drivers/button.c`)
Rockbox handles input via GPIO scanning (Matrix Keypad) or specialized controllers (iPod Clickwheel).

### 2.1 The Button Matrix Scanner (Level 4)
Physical buttons are wired in a row/column matrix to save GPIO pins. The scanning algorithm is generic, but the GPIO mapping is target-specific.

### 2.2 Deep Dive: Button Maps and Masking (Level 10)
For the Sansa Clip, the buttons are mapped to specific bits in the `button_read` return value.

#### Level 1: Logical Mapping
```c
/* firmware/target/arm/as3525/sansa-clip/button-target.h */
#define BUTTON_HOME         0x00000001
#define BUTTON_POWER        0x00000100
```

#### Level 5: Register Read
The driver reads the raw GPIO input states.
```c
/* firmware/target/arm/as3525/button-clip.c */
int gpio_a = GPIOA_PIN; /* Read Memory Mapped 0xC80B0000 */
int gpio_b = GPIOB_PIN; /* Read Memory Mapped 0xC80C0000 */
```

#### Level 8: Electrical Schematic
*   **HOME Button:** Connected to `GPIOA[1]`. Pulled HIGH (3.3V) by resistor.
*   **POWER Button:** Connected to `GPIOB[6]`.

#### Level 10: Logic Extraction
```c
int btn = 0;
/* Check GPIO A Bit 1 (Mask 0x02) */
/* Logic is Active LOW (0 = Pressed) */
if ((gpio_a & 0x02) == 0)
    btn |= BUTTON_HOME;

/* Check GPIO B Bit 6 (Mask 0x40) */
if ((gpio_b & 0x40) == 0)
    btn |= BUTTON_POWER;
```

---

## 3. Power Management (PMIC) & ADC
Rockbox is obsessed with battery life. It aggressively manages power states using custom discharge curves and charger state machines.

### 3.1 Deep Dive: Battery Voltage Curves (Level 5)
Batteries do not discharge linearly. To display an accurate percentage, Rockbox uses a lookup table (LUT) that maps voltage (mV) to percentage (0-100%).

**Source Analysis: `firmware/target/arm/as3525/sansa-clip/powermgmt-clip.c`**

```c
/* Discharge Curve (Charging Disabled) */
/* Index 0 = 0%, Index 10 = 100% */
unsigned short percent_to_volt_discharge[11] =
{
    3300, /* 0%   - 3.30V (Cutoff) */
    3653, /* 10%  - 3.65V */
    3701, /* 20%  */
    3735, /* 30%  */
    3768, /* 40%  */
    3790, /* 50%  - Nominal */
    3833, /* 60%  */
    3900, /* 70%  */
    3966, /* 80%  */
    4056, /* 90%  */
    4140  /* 100% - 4.14V (Full) */
};
```

**Logic:**
The `powermgmt` thread reads the ADC, applies a smoothing filter (moving average), and then interpolates between these points to calculate the display percentage.

### 3.2 ADC Driver (Level 10)
The ADC driver (`adc-target.h`) manages the specific hardware channel mappings.

#### Level 4: Channel Definitions
```c
/* AS3525 ADC Channel Mapping */
#define ADC_BATTERY     AS3525_ADC_CH1
#define ADC_USB_VOLT    AS3525_ADC_CH2
#define ADC_TEMP_SENS   AS3525_ADC_CH3
```

#### Level 9: The Conversion Sequence
1.  **Select Channel:** Write `0x1` (CH1) to `ADC_CTRL` register.
2.  **Start Conversion:** Set `ADC_START` bit.
3.  **Wait:** Loop until `ADC_EOC` (End of Conversion) bit is set.
4.  **Read:** Read 10-bit value from `ADC_DATA`.

#### Level 10: Analog Domain
*   **Voltage Divider:** The battery (3.7V - 4.2V) is connected to a resistor divider (e.g., 100k/100k) to bring it within the ADC's 0-2.5V reference range.
*   **Scaling:** Rockbox scales the raw reading back to millivolts: `mV = (raw * 2500 * 2) / 1024`.

### 3.3 Charging State Machine
Rockbox implements a software-controlled charging algorithm (`docs/CHARGING_ALGORITHM`).
1.  **Insertion:** Detect USB VBUS > 4.5V.
2.  **Current Limiting:** Set PMIC input current limit (100mA initially, 500mA if enumeration succeeds).
3.  **Top-Off:** When voltage reaches 4.2V, switch to Constant Voltage (CV) mode (if supported by PMIC) or pulse charging.
4.  **Safety:** Monitor temperature (via NTC thermistor on `ADC_TEMP_SENS`) and cut power if T > 45°C.
