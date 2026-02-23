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

### 1.2 Deep Dive: AS3525 DBOP LCD Driver (Level 5)
The Sansa Clip (AS3525 SoC) uses a dedicated "Data Block Output Port" (DBOP) peripheral to drive its OLED via an 8-bit parallel bus.

**Source Analysis: `firmware/target/arm/as3525/sansa-clip/lcd-clip.c`**

#### 1.2.1 Hardware Initialization
The initialization sequence involves setting up the DBOP timing and GPIO routing.

```c
int lcd_hw_init(void)
{
    /* 1. Configure DBOP Clock Divider */
    CGU_DBOP = (1<<3) | AS3525_DBOP_DIV;

    /* 2. Route DBOP signals to Pins */
    GPIOB_AFSEL = 0x08; /* Route DBOP to Pin 3 */
    GPIOC_AFSEL = 0x0f; /* Route DBOP to Pins 0-3 */

    /* 3. Configure Timing Registers (Magic Hex) */
    /* Derived from logic analyzer traces of Original Firmware */
    DBOP_CTRL      = 0x51008;    /* Control Register */
    DBOP_TIMPOL_01 = 0x6E167;    /* Timing Policy 0/1 */
    DBOP_TIMPOL_23 = 0xA167E06F; /* Timing Policy 2/3 */

    /* 4. Set GPIO Direction for Control Lines */
    GPIOA_DIR |= 0x33; /* Pins 0,1,4,5 Output */
    GPIOB_DIR |= 0x40; /* Pin 6 Output */

    return 0;
}
```

#### 1.2.2 Command Transmission (Bit-Banging vs FIFO)
The driver writes commands to the DBOP FIFO. It must manually toggle the Data/Command (D/C#) pin via GPIO.

```c
void lcd_write_command(int byte)
{
    /* 1. Wait for delay (Timing requirement) */
    volatile int i = 0;
    while(i < LCD_DELAY) i++;

    /* 2. Assert Command Mode (D/C# Low) */
    GPIOA_PIN(5) = 0;

    /* 3. Write to DBOP FIFO */
    /* The data is replicated in upper bits for 16-bit bus compatibility */
    DBOP_DOUT = (byte << 8) | byte;

    /* 4. Wait for FIFO Empty */
    while ((DBOP_STAT & (1<<10)) == 0)
        ; /* Spinwait */
}
```

#### 1.2.3 Data Transmission (The Pixel Push)
Pushing the framebuffer requires asserting Data Mode and bursting bytes.

```c
void lcd_write_data(const fb_data* p_bytes, int count)
{
    /* 1. Assert Data Mode (D/C# High) */
    GPIOA_PIN(5) = (1<<5);

    while (count--)
    {
        /* 2. Push Pixel Data */
        DBOP_DOUT = (*p_bytes << 8) | *p_bytes;
        p_bytes++;

        /* 3. Flow Control: Wait if FIFO Full */
        while ((DBOP_STAT & (1<<6)) != 0);
    }
}
```

---

## 2. Input Matrices (`firmware/drivers/button.c`)
Rockbox handles input via GPIO scanning (Matrix Keypad) or specialized controllers (iPod Clickwheel).

### 2.1 The Button Matrix Scanner (Level 4)
Physical buttons are wired in a row/column matrix to save GPIO pins. The scanning algorithm is generic, but the GPIO mapping is target-specific.

### 2.2 Deep Dive: Button Maps and Masking (Level 5)
For the Sansa Clip, the buttons are mapped to specific bits in the `button_read` return value.

**Source Analysis: `firmware/target/arm/as3525/sansa-clip/button-target.h`**

```c
/* Logical Button Map */
#define BUTTON_HOME         0x00000001
#define BUTTON_VOL_UP       0x00000002
#define BUTTON_VOL_DOWN     0x00000004
#define BUTTON_UP           0x00000008
#define BUTTON_DOWN         0x00000010
#define BUTTON_LEFT         0x00000020
#define BUTTON_RIGHT        0x00000040
#define BUTTON_SELECT       0x00000080
#define BUTTON_POWER        0x00000100

/* Combined Mask for Main Thread */
#define BUTTON_MAIN (BUTTON_HOME|BUTTON_VOL_UP|BUTTON_VOL_DOWN\
                    |BUTTON_UP|BUTTON_DOWN|BUTTON_LEFT|BUTTON_RIGHT\
                    |BUTTON_SELECT|BUTTON_POWER)
```

**The Scanning Logic (`button-clip.c`):**
The actual driver reads GPIO registers directly.
1.  **Read GPIOs:** `GPIOA_PIN` and `GPIOB_PIN` registers.
2.  **Invert Logic:** Buttons are usually Active Low (pull-up resistors).
3.  **Map to Bitmask:** Construct the integer return value.

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

/* Charge Curve (Charging Enabled - Higher due to internal resistance) */
unsigned short percent_to_volt_charge[11] =
{
    3427, 3786, 3842, 3877, 3896, 3924, 3971, 4028, 4084, 4161, 4190
};
```

**Logic:**
The `powermgmt` thread reads the ADC, applies a smoothing filter (moving average), and then interpolates between these points to calculate the display percentage.

### 3.2 ADC Driver (Level 5)
The ADC driver (`adc-target.h`) manages the specific hardware channel mappings.
*   **`ADC_BATTERY`**: The channel connected to the battery divider.
*   **`ADC_BUTTONS`**: Some devices (like Wired Remotes) use resistor ladders on an ADC pin for buttons.

```c
/* AS3525 ADC Channel Mapping */
#define ADC_BATTERY     AS3525_ADC_CH1
#define ADC_USB_VOLT    AS3525_ADC_CH2
#define ADC_TEMP_SENS   AS3525_ADC_CH3
```

### 3.3 Charging State Machine
Rockbox implements a software-controlled charging algorithm (`docs/CHARGING_ALGORITHM`).
1.  **Insertion:** Detect USB VBUS > 4.5V.
2.  **Current Limiting:** Set PMIC input current limit (100mA initially, 500mA if enumeration succeeds).
3.  **Top-Off:** When voltage reaches 4.2V, switch to Constant Voltage (CV) mode (if supported by PMIC) or pulse charging.
4.  **Safety:** Monitor temperature (via NTC thermistor on `ADC_TEMP_SENS`) and cut power if T > 45°C.
