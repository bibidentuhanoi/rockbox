#ifndef _BUTTON_TARGET_H_
#define _BUTTON_TARGET_H_

/* Logical button keycodes for ESP32 */
#define BUTTON_UP           0x00000001
#define BUTTON_DOWN         0x00000002
#define BUTTON_LEFT         0x00000004
#define BUTTON_RIGHT        0x00000008
#define BUTTON_USER         0x00000010
#define BUTTON_MENU         0x00000020
#define BUTTON_BACK         0x00000040
#define BUTTON_POWER        0x00000080
#define BUTTON_SELECT       0x00000100

#define BUTTON_MAIN         0x000001FF

/* Software power-off */
#define POWEROFF_BUTTON BUTTON_POWER
#define POWEROFF_COUNT  10

#endif /* _BUTTON_TARGET_H_ */
