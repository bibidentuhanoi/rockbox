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

### 1.2 Deep Dive: AS3525 DBOP LCD Driver (Level 12 Depth)
The Sansa Clip (AS3525 SoC) uses a dedicated "Data Block Output Port" (DBOP) peripheral to drive its OLED via an 8-bit parallel bus. This section dissects the driver stack from the high-level API down to the photon emission physics.

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

```text
Clock: 24MHz (41.6ns)
        __    __    __    __    __    __    __
CLK  __|  |__|  |__|  |__|  |__|  |__|  |__|  |__
     _____________________________________________
CS#
     __________                         __________
WR#            |_______________________|
               <----- 0x67 (4us) ----->
     ____________________ ________________________
D0-7 --------------------<__0xAF__________________>
```
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

#### Level 11: Signal Integrity & Timing Constraints
At Level 11, we consider the signal propagation delay.
*   **24MHz Clock Period:** ~41.6ns.
*   **Pulse Width:** `0x67` cycles = 103 * 41.6ns = ~4.28us.
*   **SSD1329 Spec:** Requires write pulse width > 60ns. Rockbox is conservatively slow here to prevent corruption due to trace capacitance.
*   **Capacitance:** The Flex PCB connector adds ~10-20pF per line. The AS3525 GPIO drive strength must be sufficient to toggle this capacitance within the setup/hold time.

#### Level 12: The Photon Emission Logic
When the Gate Driver selects Row N and the Source Driver applies voltage to Column M (based on `framebuffer` content):
1.  **Current Flow:** Current flows through the Organic LED material.
2.  **Exciton Formation:** Holes and electrons recombine.
3.  **Emission:** Energy is released as photons.
4.  **Decay:** The "ghosting" effect is negligible on OLEDs compared to LCDs, allowing the "1-bit Vertical" refresh strategy to be tear-free even without VSYNC synchronization on this device.

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

#### Level 11: The Time-Domain Logic (Debounce)
In `firmware/drivers/button.c`, `button_tick()` is called every kernel tick (e.g., 10ms).
*   **State History:** The `lastbtn` static variable holds the previous state.
*   **Logic:**
    ```c
    int diff = btn ^ lastbtn;
    if (diff) {
        /* State changed. Post EVENT_BUTTON to queue. */
        button_queue_post(btn);
    }
    ```
*   **Acceleration:** For Repeat keys (Volume), a counter `repeat_speed` decrements on every tick while the button is held. When it hits 0, a new event is generated, and `repeat_speed` is reset to a smaller value (accelerating from 160ms -> 50ms repeat rate).

#### Level 12: The Physics of Contact Bounce
When the metal dome of the HOME button snaps down:
1.  **Make:** The contacts touch.
2.  **Bounce:** The dome performs damped harmonic oscillation, breaking contact repeatedly for ~1-5ms.
3.  **Settle:** Constant contact is established.
4.  **Rockbox Handling:** Since `button_tick` runs at ~100Hz (10ms period), it inherently low-pass filters these sub-10ms bounces. The first tick sees the "Make". The bounces happen *between* ticks and are invisible to the software.

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

#### Level 11: The I2C Transaction State Machine (PMIC)
The AS3514 PMIC (used in some variants) is controlled via I2C.
1.  **Bit-Banging:** Rockbox manually toggles GPIOs for SDA/SCL.
    ```c
    /* firmware/target/arm/as3525/fmradio-i2c-as3525.c */
    void i2c_bit(bool level) {
        SDA_OUT(level);
        udelay(2); /* Setup time */
        SCL_OUT(1);
        udelay(2); /* Hold time */
        SCL_OUT(0);
    }
    ```
2.  **ACK Polling:** After sending the register address (`0x22` for Charger), the driver switches SDA to Input and pulses SCL to read the ACK bit from the PMIC.

#### Level 12: The Electrochemical Model
The `percent_to_volt_discharge` table implicitly models the Li-Ion chemistry (LiCoO2).
*   **3.7V Plateau:** The flat region of the curve (30% to 70%) corresponds to the phase transition plateau of the cathode material.
*   **Temperature Coefficient:** Rockbox monitors `ADC_TEMP_SENS`. While the legacy code primarily uses this for safety (cut-off > 45°C), advanced patches use it to adjust the voltage curve, as Li-Ion voltage drops significantly at low temperatures (increasing internal resistance).
*   **Peukert's Law:** The "Charge" vs "Discharge" tables account for the IR drop ($V_{term} = V_{ocv} - I \times R_{internal}$). When charging, the terminal voltage is higher ($V_{term} = V_{ocv} + I \times R_{internal}$), necessitating the `percent_to_volt_charge` table offset to prevent the UI from jumping to "100%" immediately upon plugging in.
