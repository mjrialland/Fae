#include "keyboard.h"
#include "io.h"

#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static int kb_head = 0;
static int kb_tail = 0;

static int shift_active = 0;

static const char kbd_us[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',    /* 9 */
  '9', '0', '-', '=', '\b',    /* Backspace */
  '\t',            /* Tab */
  'q', 'w', 'e', 'r',    /* 19 */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',    /* Enter key */
    0,            /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',    /* 39 */
 '\'', '`',   0,        /* Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,                /* Right shift */
  '*',
    0,    /* Alt */
  ' ',    /* Space bar */
    0,    /* Caps lock */
    0,    /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,    /* < ... F10 */
    0,    /* 69 - Num lock*/
    0,    /* Scroll Lock */
    0,    /* Home key */
    0,    /* Up Arrow */
    0,    /* Page Up */
  '-',
    0,    /* Left Arrow */
    0,
    0,    /* Right Arrow */
  '+',
    0,    /* 79 - End key*/
    0,    /* Down Arrow */
    0,    /* Page Down */
    0,    /* Insert Key */
    0,    /* Delete Key */
    0,   0,   0,
    0,    /* F11 Key */
    0,    /* F12 Key */
    0,    /* All other keys are undefined */
};

static const char kbd_us_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',    /* 9 */
  '(', ')', '_', '+', '\b',    /* Backspace */
  '\t',            /* Tab */
  'Q', 'W', 'E', 'R',    /* 19 */
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',    /* Enter key */
    0,            /* 29   - Control */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',    /* 39 */
 '"', '~',   0,        /* Left shift */
 '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0,                /* Right shift */
  '*',
    0,    /* Alt */
  ' ',    /* Space bar */
    0,    /* Caps lock */
};

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    
    // Check if shift is pressed or released
    if (scancode == 0x2A || scancode == 0x36) {
        shift_active = 1;
    } else if (scancode == (0x2A | 0x80) || scancode == (0x36 | 0x80)) {
        shift_active = 0;
    }
    
    // If it's a press event (highest bit not set)
    if (!(scancode & 0x80)) {
        char ascii = shift_active ? kbd_us_shift[scancode] : kbd_us[scancode];
        if (ascii != 0) {
            // Write to buffer if not full
            int next = (kb_head + 1) % KB_BUFFER_SIZE;
            if (next != kb_tail) {
                kb_buffer[kb_head] = ascii;
                kb_head = next;
            }
        }
    }
    
    // Send End Of Interrupt (EOI) to PIC
    outb(0x20, 0x20);
}

int keyboard_has_char(void) {
    return kb_head != kb_tail;
}

char keyboard_get_char(void) {
    while (!keyboard_has_char()) {
        __asm__ volatile ("hlt"); // Wait for interrupt
    }
    char c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return c;
}
