#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void keyboard_handler(void);
char keyboard_get_char(void);
int keyboard_has_char(void);

#endif
