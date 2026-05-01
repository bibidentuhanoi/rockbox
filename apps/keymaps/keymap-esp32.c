/* Button Code Definitions for ESP32 target */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "action.h"
#include "button.h"
#include "settings.h"

static const struct button_mapping button_context_standard[] = {
    { ACTION_STD_PREV,           BUTTON_UP,                   BUTTON_NONE },
    { ACTION_STD_PREVREPEAT,     BUTTON_UP|BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_STD_NEXT,           BUTTON_DOWN,                 BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT,     BUTTON_DOWN|BUTTON_REPEAT,   BUTTON_NONE },
    { ACTION_STD_CANCEL,         BUTTON_LEFT,                 BUTTON_NONE },
    { ACTION_STD_CANCEL,         BUTTON_BACK,                 BUTTON_NONE },
    { ACTION_STD_CANCEL,         BUTTON_POWER,                BUTTON_NONE },
    { ACTION_STD_OK,             BUTTON_SELECT,               BUTTON_NONE },
    { ACTION_STD_OK,             BUTTON_RIGHT,                BUTTON_NONE },
    { ACTION_STD_QUICKSCREEN,    BUTTON_MENU|BUTTON_REPEAT,   BUTTON_NONE },
    { ACTION_STD_CONTEXT,        BUTTON_SELECT|BUTTON_REPEAT, BUTTON_SELECT },
    { ACTION_STD_MENU,           BUTTON_MENU|BUTTON_REL,      BUTTON_MENU },
    LAST_ITEM_IN_LIST
};

static const struct button_mapping button_context_mainmenu[] = {
    { ACTION_TREE_WPS, BUTTON_MENU|BUTTON_REL, BUTTON_MENU },
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_TREE),
};

static const struct button_mapping button_context_wps[] = {
    { ACTION_WPS_PLAY,   BUTTON_SELECT,              BUTTON_NONE },
    { ACTION_WPS_STOP,   BUTTON_POWER,               BUTTON_NONE },
    { ACTION_WPS_BROWSE, BUTTON_BACK,                BUTTON_NONE },
    { ACTION_WPS_MENU,   BUTTON_MENU|BUTTON_REL,     BUTTON_MENU },
    { ACTION_WPS_SEEKFWD,  BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_WPS_SEEKBACK, BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE },
    { ACTION_WPS_SKIPNEXT, BUTTON_RIGHT|BUTTON_REL,    BUTTON_RIGHT },
    { ACTION_WPS_SKIPPREV, BUTTON_LEFT|BUTTON_REL,     BUTTON_LEFT },
    { ACTION_WPS_VOLUP,    BUTTON_UP|BUTTON_REL,       BUTTON_NONE },
    { ACTION_WPS_VOLUP,    BUTTON_UP|BUTTON_REPEAT,    BUTTON_NONE },
    { ACTION_WPS_VOLDOWN,  BUTTON_DOWN|BUTTON_REL,     BUTTON_NONE },
    { ACTION_WPS_VOLDOWN,  BUTTON_DOWN|BUTTON_REPEAT,  BUTTON_NONE },
    LAST_ITEM_IN_LIST
};

static const struct button_mapping button_context_tree[] = {
    { ACTION_TREE_WPS,       BUTTON_MENU|BUTTON_REL,    BUTTON_MENU },
    { ACTION_TREE_STOP,      BUTTON_POWER|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_TREE_PGLEFT,    BUTTON_LEFT,               BUTTON_NONE },
    { ACTION_TREE_PGRIGHT,   BUTTON_RIGHT,              BUTTON_NONE },
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping button_context_settings[] = {
    { ACTION_SETTINGS_INC,       BUTTON_RIGHT,             BUTTON_NONE },
    { ACTION_SETTINGS_INCREPEAT, BUTTON_RIGHT|BUTTON_REPEAT,BUTTON_NONE },
    { ACTION_SETTINGS_DEC,       BUTTON_LEFT,              BUTTON_NONE },
    { ACTION_SETTINGS_DECREPEAT, BUTTON_LEFT|BUTTON_REPEAT, BUTTON_NONE },
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping *button_context_esp32[] = {
    [CONTEXT_STD]      = button_context_standard,
    [CONTEXT_WPS]      = button_context_wps,
    [CONTEXT_MAINMENU] = button_context_mainmenu,
    [CONTEXT_TREE]     = button_context_tree,
    [CONTEXT_SETTINGS] = button_context_settings,
};

const struct button_mapping *get_context_mapping(int context)
{
    if (context >= 0 && context < (int)ARRAYLEN(button_context_esp32)
            && button_context_esp32[context])
        return button_context_esp32[context];
    return button_context_standard;
}
