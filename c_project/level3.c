#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>

#define TEXOR_VERSION "0.0.1"
#define TEXOR_TAB_STOP 4
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

typedef struct erow {
  int size;
  int rendered_size;
  char *characters;
  char *rendered_characters;
} erow;

struct editorConfig {
  int file_position_x, file_position_y;
  int screen_position_x;
  int row_offset;
  int column_offset;
  int screen_rows;
  int screen_columns;
  int number_of_rows;
  erow *row;
  char *filename;
  char status_message[80];
  time_t status_message_time;
  struct termios orig_termios;
};
struct editorConfig E;

void editorSetStatusMessage(const char *fmt, ...);

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);
  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->characters[j] == '\t') tabs++;
  free(row->rendered_characters);
  row->rendered_characters = malloc(row->size + tabs * (TEXOR_TAB_STOP - 1) + 1);
  int index = 0;
  for (j = 0; j < row->size; j++) {
    if (row->characters[j] == '\t') {
      row->rendered_characters[index++] = ' ';
      while (index % TEXOR_TAB_STOP != 0) row->rendered_characters[index++] = ' ';
    } else {
      row->rendered_characters[index++] = row->characters[j];
    }
  }
  row->rendered_characters[index] = '\0';
  row->rendered_size = index;
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.number_of_rows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.number_of_rows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.number_of_rows - at));
  E.row[at].size = len;
  E.row[at].characters = malloc(len + 1);
  memcpy(E.row[at].characters, s, len);
  E.row[at].characters[len] = '\0';
  E.row[at].rendered_size = 0;
  E.row[at].rendered_characters = NULL;
  editorUpdateRow(&E.row[at]);
  E.number_of_rows++;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.number_of_rows, line, linelen);
  }
  free(line);
  fclose(fp);
}

struct abuf {
  char *b;
  int len;
};
#define ABUF_INIT {NULL, 0}
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}
void abFree(struct abuf *ab) {
  free(ab->b);
}

int editorRowFilePositionXToScreenPositionX(erow *row, int file_position_x) {
  int screen_position_x = 0;
  int j;
  for (j = 0; j < file_position_x; j++) {
    if (row->characters[j] == '\t')
      screen_position_x += (TEXOR_TAB_STOP - 1) - (screen_position_x % TEXOR_TAB_STOP);
    screen_position_x++;
  }
  return screen_position_x;
}

void editorScroll() {
  E.screen_position_x = 0;
  if (E.file_position_y < E.number_of_rows) {
    E.screen_position_x = editorRowFilePositionXToScreenPositionX(&E.row[E.file_position_y], E.file_position_x);
  }
  if (E.file_position_y < E.row_offset) {
    E.row_offset = E.file_position_y;
  }
  if (E.file_position_y >= E.row_offset + E.screen_rows) {
    E.row_offset = E.file_position_y - E.screen_rows + 1;
  }
  if (E.screen_position_x < E.column_offset) {
    E.column_offset = E.screen_position_x;
  }
  if (E.screen_position_x >= E.column_offset + E.screen_columns) {
    E.column_offset = E.screen_position_x - E.screen_columns + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screen_rows; y++) {
    int filerow = y + E.row_offset;
    if (filerow >= E.number_of_rows) {
        if (E.number_of_rows == 0 && y == E.screen_rows / 3) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome), "Texor Viewer -- version %s", TEXOR_VERSION);
            if (welcomelen > E.screen_columns) welcomelen = E.screen_columns;
            int padding = (E.screen_columns - welcomelen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }
            while (padding--) abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcomelen);
        } else {
            abAppend(ab, "~", 1);
        }
    } else {
      int len = E.row[filerow].rendered_size - E.column_offset;
      if (len < 0) len = 0;
      if (len > E.screen_columns) len = E.screen_columns;
      abAppend(ab, &E.row[filerow].rendered_characters[E.column_offset], len);
    }
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  char *display_filename = "[No Name]"; // 默认显示 [No Name]

  if (E.filename != NULL && strlen(E.filename) <= 20) {
    display_filename = E.filename;
  }

  int len = snprintf(status, sizeof(status), "%s - %d lines",
      display_filename, E.number_of_rows);

  int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d/%d",
      E.file_position_y + 1, E.file_position_x + 1, E.number_of_rows);

  if (len > E.screen_columns) len = E.screen_columns;
  abAppend(ab, status, len);
  while (len < E.screen_columns) {
    if (E.screen_columns - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.status_message);
  if (msglen > E.screen_columns) msglen = E.screen_columns;
  if (msglen && time(NULL) - E.status_message_time < 5)
    abAppend(ab, E.status_message, msglen);
}

void editorRefreshScreen() {
  editorScroll();
  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);
  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.file_position_y - E.row_offset) + 1, (E.screen_position_x - E.column_offset) + 1);
  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6);
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.status_message, sizeof(E.status_message), fmt, ap);
  va_end(ap);
  E.status_message_time = time(NULL);
}

void editorMoveCursor(int key) {
  erow *row = (E.file_position_y >= E.number_of_rows) ? NULL : &E.row[E.file_position_y];
  switch (key) {
    case ARROW_LEFT:
      if (E.file_position_x != 0) {
        E.file_position_x--;
      } else if (E.file_position_y > 0) {
        E.file_position_y--;
        E.file_position_x = E.row[E.file_position_y].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.file_position_x < row->size) {
        E.file_position_x++;
      } else if (row && E.file_position_x == row->size) {
        E.file_position_y++;
        E.file_position_x = 0;
      }
      break;
    case ARROW_UP:
      if (E.file_position_y != 0) E.file_position_y--;
      break;
    case ARROW_DOWN:
      if (E.file_position_y < E.number_of_rows - 1) E.file_position_y++;
      break;
  }
  row = (E.file_position_y >= E.number_of_rows) ? NULL : &E.row[E.file_position_y];
  int rowlen = row ? row->size : 0;
  if (E.file_position_x > rowlen) {
    E.file_position_x = rowlen;
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();
  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
    case HOME_KEY:
      E.file_position_x = 0;
      break;
    case END_KEY:
      if (E.file_position_y < E.number_of_rows)
        E.file_position_x = E.row[E.file_position_y].size;
      break;
    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.file_position_y = E.row_offset;
        } else if (c == PAGE_DOWN) {
          E.file_position_y = E.row_offset + E.screen_rows - 1;
          if (E.file_position_y > E.number_of_rows) E.file_position_y = E.number_of_rows;
        }
        int times = E.screen_rows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

void initEditor() {
  E.file_position_x = 0;
  E.file_position_y = 0;
  E.screen_position_x = 0;
  E.row_offset = 0;
  E.column_offset = 0;
  E.number_of_rows = 0;
  E.row = NULL;
  E.filename = NULL;
  E.status_message[0] = '\0';
  E.status_message_time = 0;
  if (getWindowSize(&E.screen_rows, &E.screen_columns) == -1) die("getWindowSize");
  E.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  } else {
    editorSetStatusMessage("HELP: Ctrl-Q = quit");
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}