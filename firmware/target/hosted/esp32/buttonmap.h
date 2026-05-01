#ifndef __BUTTONMAP_H__
#define __BUTTONMAP_H__

struct button_map {
    int button, x, y, radius;
    char *description;
};

extern struct button_map bm[];
int xy2button(int x, int y);

#ifdef HAVE_TOUCHSCREEN
int key_to_touch(int keyboard_button, unsigned int mouse_coords);
#endif

#endif /* __BUTTONMAP_H__ */
