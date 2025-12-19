/**
 * Punto Switcher for Linux (C Version)
 * Manual mode - triggered by Pause key
 * Features:
 *   - Invert layout for current/last word (preserves case)
 *   - Invert layout for selected text via clipboard
 *   - Configurable hotkey via /etc/punto/config.yaml
 */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_WORD_LEN 256
#define CONFIG_PATH "/etc/punto/config.yaml"

// Configuration with defaults
static int cfg_hotkey_modifier = KEY_LEFTCTRL;
static int cfg_hotkey_key = KEY_GRAVE;
static int cfg_delay_key_press = 20000;      // 20ms
static int cfg_delay_layout_switch = 100000; // 100ms
static int cfg_delay_retype = 3000;          // 3ms

static const char SCANCODE_TO_CHAR[256] = {
    [KEY_Q] = 'q',
    [KEY_W] = 'w',
    [KEY_E] = 'e',
    [KEY_R] = 'r',
    [KEY_T] = 't',
    [KEY_Y] = 'y',
    [KEY_U] = 'u',
    [KEY_I] = 'i',
    [KEY_O] = 'o',
    [KEY_P] = 'p',
    [KEY_A] = 'a',
    [KEY_S] = 's',
    [KEY_D] = 'd',
    [KEY_F] = 'f',
    [KEY_G] = 'g',
    [KEY_H] = 'h',
    [KEY_J] = 'j',
    [KEY_K] = 'k',
    [KEY_L] = 'l',
    [KEY_Z] = 'z',
    [KEY_X] = 'x',
    [KEY_C] = 'c',
    [KEY_V] = 'v',
    [KEY_B] = 'b',
    [KEY_N] = 'n',
    [KEY_M] = 'm',
    // Additional keys that should be part of words
    [KEY_LEFTBRACE] = '[',
    [KEY_RIGHTBRACE] = ']',
    [KEY_SEMICOLON] = ';',
    [KEY_APOSTROPHE] = '\'',
    [KEY_GRAVE] = '`',
    [KEY_SLASH] = '/',
    [KEY_1] = '1',
    [KEY_2] = '2',
    [KEY_3] = '3',
    [KEY_4] = '4',
    [KEY_5] = '5',
    [KEY_6] = '6',
    [KEY_7] = '7',
    [KEY_8] = '8',
    [KEY_9] = '9',
    [KEY_0] = '0',
    [KEY_MINUS] = '-',
    [KEY_EQUAL] = '=',
    [KEY_BACKSLASH] = '\\',
    // These are Russian letters б and ю!
    [KEY_COMMA] = ',',
    [KEY_DOT] = '.',
};

// Word buffer: scancode + shift state
static int word_buffer[MAX_WORD_LEN];
static bool word_shift[MAX_WORD_LEN];
static int word_len = 0;

// Last word for repeated Pause
static int last_word_buffer[MAX_WORD_LEN];
static bool last_word_shift[MAX_WORD_LEN];
static int last_word_len = 0;

// Trailing spaces/tabs buffer
static int trailing_buffer[MAX_WORD_LEN];
static int trailing_len = 0;

// Current Shift state
static bool shift_pressed = false;

/**
 * Convert key name to keycode
 */
static int key_name_to_code(const char *name) {
  if (strcmp(name, "leftctrl") == 0)
    return KEY_LEFTCTRL;
  if (strcmp(name, "rightctrl") == 0)
    return KEY_RIGHTCTRL;
  if (strcmp(name, "leftalt") == 0)
    return KEY_LEFTALT;
  if (strcmp(name, "rightalt") == 0)
    return KEY_RIGHTALT;
  if (strcmp(name, "leftshift") == 0)
    return KEY_LEFTSHIFT;
  if (strcmp(name, "rightshift") == 0)
    return KEY_RIGHTSHIFT;
  if (strcmp(name, "leftmeta") == 0)
    return KEY_LEFTMETA;
  if (strcmp(name, "rightmeta") == 0)
    return KEY_RIGHTMETA;
  if (strcmp(name, "grave") == 0)
    return KEY_GRAVE;
  if (strcmp(name, "space") == 0)
    return KEY_SPACE;
  if (strcmp(name, "tab") == 0)
    return KEY_TAB;
  if (strcmp(name, "backslash") == 0)
    return KEY_BACKSLASH;
  if (strcmp(name, "capslock") == 0)
    return KEY_CAPSLOCK;
  return -1;
}

/**
 * Load configuration from YAML file
 */
static void load_config(void) {
  FILE *f = fopen(CONFIG_PATH, "r");
  if (!f)
    return; // Use defaults

  char line[256];
  char current_section[64] = "";

  while (fgets(line, sizeof(line), f)) {
    // Skip comments and empty lines
    char *p = line;
    while (*p && isspace(*p))
      p++;
    if (*p == '#' || *p == '\0' || *p == '\n')
      continue;

    // Check for section (e.g., "hotkey:")
    if (strstr(line, "hotkey:") && !strstr(line, "  ")) {
      strcpy(current_section, "hotkey");
      continue;
    }
    if (strstr(line, "delays:") && !strstr(line, "  ")) {
      strcpy(current_section, "delays");
      continue;
    }

    // Parse key: value
    char *colon = strchr(p, ':');
    if (!colon)
      continue;

    *colon = '\0';
    char *key = p;
    char *value = colon + 1;

    // Trim key
    while (*key && isspace(*key))
      key++;
    char *key_end = key + strlen(key) - 1;
    while (key_end > key && isspace(*key_end))
      *key_end-- = '\0';

    // Trim value
    while (*value && isspace(*value))
      value++;
    char *value_end = value + strlen(value) - 1;
    while (value_end > value && (isspace(*value_end) || *value_end == '\n'))
      *value_end-- = '\0';

    if (strcmp(current_section, "hotkey") == 0) {
      if (strcmp(key, "modifier") == 0) {
        int code = key_name_to_code(value);
        if (code >= 0)
          cfg_hotkey_modifier = code;
      } else if (strcmp(key, "key") == 0) {
        int code = key_name_to_code(value);
        if (code >= 0)
          cfg_hotkey_key = code;
      }
    } else if (strcmp(current_section, "delays") == 0) {
      int ms = atoi(value);
      if (ms > 0) {
        if (strcmp(key, "key_press") == 0)
          cfg_delay_key_press = ms * 1000;
        else if (strcmp(key, "layout_switch") == 0)
          cfg_delay_layout_switch = ms * 1000;
        else if (strcmp(key, "retype") == 0)
          cfg_delay_retype = ms * 1000;
      }
    }
  }

  fclose(f);
}

static void emit_event(const struct input_event *ev) {
  if (fwrite(ev, sizeof(*ev), 1, stdout) != 1)
    exit(1);
  fflush(stdout);
}

static void send_key(int code, int value) {
  struct input_event ev = {0};
  ev.type = EV_KEY;
  ev.code = code;
  ev.value = value;
  emit_event(&ev);

  ev.type = EV_SYN;
  ev.code = SYN_REPORT;
  ev.value = 0;
  emit_event(&ev);
}

static void send_backspace(int count) {
  for (int i = 0; i < count; i++) {
    send_key(KEY_BACKSPACE, 1);
    send_key(KEY_BACKSPACE, 0);
    usleep(cfg_delay_retype);
  }
}

static void retype_buffer_with_case(const int *buf, const bool *shifts,
                                    int len) {
  for (int i = 0; i < len; i++) {
    if (shifts[i]) {
      send_key(KEY_LEFTSHIFT, 1);
      usleep(cfg_delay_retype / 2);
    }
    send_key(buf[i], 1);
    send_key(buf[i], 0);
    if (shifts[i]) {
      usleep(cfg_delay_retype / 2);
      send_key(KEY_LEFTSHIFT, 0);
    }
    usleep(cfg_delay_retype);
  }
}

/**
 * Retype simple scancodes without shift (for spaces/tabs)
 */
static void retype_simple(const int *buf, int len) {
  for (int i = 0; i < len; i++) {
    send_key(buf[i], 1);
    send_key(buf[i], 0);
    usleep(cfg_delay_retype);
  }
}

/**

 * Switch layout using configured hotkey (fallback)
 */
static void switch_layout_via_hotkey(void) {
  usleep(cfg_delay_key_press);
  send_key(cfg_hotkey_modifier, 1);
  usleep(cfg_delay_key_press);
  send_key(cfg_hotkey_key, 1);
  usleep(cfg_delay_key_press + 10000);
  send_key(cfg_hotkey_key, 0);
  usleep(cfg_delay_key_press);
  send_key(cfg_hotkey_modifier, 0);
  usleep(cfg_delay_layout_switch);
}

/**
 * Switch layout - use hotkey simulation
 */
static void switch_layout(void) { switch_layout_via_hotkey(); }

/**
 * Release all modifiers to prevent interference with retyping
 */
static void release_modifiers(void) {
  send_key(KEY_LEFTSHIFT, 0);
  send_key(KEY_RIGHTSHIFT, 0);
  send_key(KEY_LEFTCTRL, 0);
  send_key(KEY_RIGHTCTRL, 0);
  send_key(KEY_LEFTALT, 0);
  send_key(KEY_RIGHTALT, 0);
  send_key(KEY_LEFTMETA, 0);
  send_key(KEY_RIGHTMETA, 0);
  usleep(cfg_delay_key_press);
}

/**
 * Invert selected text via external script
 * Script handles copy/paste with terminal detection
 */
static void invert_selection(void) {
  release_modifiers();
  usleep(cfg_delay_key_press);

  // Script will handle copy, invert, and paste
  if (system("/usr/local/bin/punto-invert") != 0) {
    return;
  }
}

/**
 * Invert selected text case via external script
 */
static void invert_selection_case(void) {
  release_modifiers();
  usleep(cfg_delay_key_press);

  // Script will handle copy, swapcase, and paste
  if (system("/usr/local/bin/punto-case-invert") != 0) {
    return;
  }
}

static void invert_case_last_word(const int *buf, const bool *shifts, int w_len,
                                  const int *trail_buf, int t_len) {
  release_modifiers();

  if (w_len > 0) {
    memcpy(last_word_buffer, buf, w_len * sizeof(int));
    memcpy(last_word_shift, shifts, w_len * sizeof(bool));
    last_word_len = w_len;
  }

  if (last_word_len == 0)
    return;

  send_backspace(last_word_len + t_len);

  // Create inverted shift buffer
  bool inverted_shifts[MAX_WORD_LEN];
  for (int i = 0; i < last_word_len; i++) {
    inverted_shifts[i] = !last_word_shift[i];
  }

  retype_buffer_with_case(last_word_buffer, inverted_shifts, last_word_len);

  if (t_len > 0 && trail_buf) {
    retype_simple(trail_buf, t_len);
  }
}

static void perform_manual_switch(const int *buf, const bool *shifts, int w_len,
                                  const int *trail_buf, int t_len) {
  release_modifiers();

  if (w_len > 0) {
    memcpy(last_word_buffer, buf, w_len * sizeof(int));
    memcpy(last_word_shift, shifts, w_len * sizeof(bool));
    last_word_len = w_len;
  }

  send_backspace(last_word_len + t_len);
  switch_layout();
  retype_buffer_with_case(last_word_buffer, last_word_shift, last_word_len);
  if (t_len > 0 && trail_buf) {
    retype_simple(trail_buf, t_len);
  }
}

int main(void) {
  load_config();

  struct input_event ev;
  setbuf(stdin, NULL);
  setbuf(stdout, NULL);

  bool ctrl = false, alt = false, meta = false;

  while (fread(&ev, sizeof(ev), 1, stdin) == 1) {
    if (ev.type != EV_KEY) {
      emit_event(&ev);
      continue;
    }

    int code = ev.code;
    int value = ev.value;

    // Track modifiers
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
      shift_pressed = (value != 0);
      emit_event(&ev);
      continue;
    }
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
      ctrl = (value != 0);
      emit_event(&ev);
      continue;
    }
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) {
      alt = (value != 0);
      emit_event(&ev);
      continue;
    }
    if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) {
      meta = (value != 0);
      emit_event(&ev);
      continue;
    }

    // Only handle key press (value=1), not release (0) or repeat (2)
    if (value != 1) {
      emit_event(&ev);
      continue;
    }

    // Backspace
    if (code == KEY_BACKSPACE) {
      if (word_len > 0)
        word_len--;
      emit_event(&ev);
      continue;
    }

    // SHIFT+PAUSE = Invert selection layout
    if (code == KEY_PAUSE && shift_pressed) {
      invert_selection();
      continue;
    }

    // CTRL+PAUSE = Invert last word case
    if (code == KEY_PAUSE && ctrl) {
      if (word_len >= 1) {
        invert_case_last_word(word_buffer, word_shift, word_len, NULL, 0);
        word_len = 0;
        trailing_len = 0;
      } else if (last_word_len >= 1) {
        invert_case_last_word(last_word_buffer, last_word_shift, last_word_len,
                              trailing_buffer, trailing_len);
      }
      continue;
    }

    // ALT+PAUSE = Invert selection case
    if (code == KEY_PAUSE && alt) {
      invert_selection_case();
      continue;
    }

    // PAUSE = Manual switch (layout)
    if (code == KEY_PAUSE) {
      if (word_len >= 1) {
        perform_manual_switch(word_buffer, word_shift, word_len, NULL, 0);
        word_len = 0;
        trailing_len = 0;
      } else if (last_word_len >= 1) {
        perform_manual_switch(last_word_buffer, last_word_shift, last_word_len,
                              trailing_buffer, trailing_len);
      }
      continue;
    }

    // Bypass for system hotkeys (Ctrl+C, etc.)
    // We only reset buffer if a non-modifier key is pressed ALONG with a
    // modifier
    if (ctrl || alt || meta) {
      word_len = 0;
      emit_event(&ev);
      continue;
    }

    // Delimiters (only keys that are ALWAYS delimiters, not letters in other
    // layouts)
    if (code == KEY_SPACE || code == KEY_TAB) {
      if (word_len > 0) {
        memcpy(last_word_buffer, word_buffer, word_len * sizeof(int));
        memcpy(last_word_shift, word_shift, word_len * sizeof(bool));
        last_word_len = word_len;
        word_len = 0;
        trailing_len = 0;
      }
      if (trailing_len < MAX_WORD_LEN - 1) {
        trailing_buffer[trailing_len++] = code;
      }
      emit_event(&ev);
      continue;
    }

    if (code == KEY_ENTER || code == KEY_KPENTER) {
      word_len = 0;
      last_word_len = 0;
      trailing_len = 0;
      emit_event(&ev);
      continue;
    }

    // Letter keys
    if (code > 0 && code < 256 && SCANCODE_TO_CHAR[code]) {
      if (word_len == 0) {
        trailing_len = 0;
      }
      if (word_len < MAX_WORD_LEN - 1) {
        word_buffer[word_len] = code;
        word_shift[word_len] = shift_pressed;
        word_len++;
      }
      emit_event(&ev);
      continue;
    }

    // Other keys - check if it's a navigation/function key
    bool is_nav_key =
        (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP ||
         code == KEY_DOWN || code == KEY_HOME || code == KEY_END ||
         code == KEY_PAGEUP || code == KEY_PAGEDOWN || code == KEY_INSERT ||
         code == KEY_DELETE);
    bool is_fkey = (code >= KEY_F1 && code <= KEY_F12);

    // Reset buffer on unknown keys (but not navigation/function keys)
    if (!is_nav_key && !is_fkey) {
      word_len = 0;
    } else if (is_nav_key) {
      word_len = 0;
      last_word_len = 0;
      trailing_len = 0;
    }
    emit_event(&ev);
  }

  return 0;
}
