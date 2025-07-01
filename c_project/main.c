#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "terminal.h"

#define TEXOR_TAG "SeedClass_Test by Xian Li"
#define TEXOR_TAB_STOP 8
#define TEXOR_QUIT_TIMES 2
#define INPUT_BUFSIZE 128

// 一个好用的宏，用于计算Ctrl键与字母键组合后的ASCII码。
// 利用了大多数终端中Ctrl组合键的值等于对应字母ASCII码的低5位这一特性。
// `& 0x1f` 的结果范围是 0-31。
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
  int number_of_rows;       // 文件当前总行数。
  erow *row;                // 代表文件的一行。
  int dirty;                // 脏标志,大于0表示文件内容自上次保存后已被修改。
  char *filename;
  char status_message[80];  // 临时消息
  time_t status_message_time; // 状态栏消息时间戳，用于自动消失。
};

struct editorConfig E;

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int)); // 显示用户输入提示框并获取输入的函数原型。
void editorSaveAs();


int editorReadKey() {
  int nread;
  char c;
  // 阻塞式读取循环
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }
  if (c == '\x1b') {  //ESC
    char seq[3];
    //后续没有字节
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';
    // 判断是否是 CSI序列
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b'; // 读取序列的最后一个字符 '~'。
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
        // 如果第三个字符是字母，通常是箭头
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



int editorRowFilePositionXToScreenPositionX(erow *row, int file_position_x) {
  int screen_position_x = 0;
  // 遍历到目标逻辑位置前的所有字符
  for (int j = 0; j < file_position_x; j++) {
    if (row->characters[j] == '\t')
      screen_position_x += (TEXOR_TAB_STOP - 1) - (screen_position_x % TEXOR_TAB_STOP);
    screen_position_x++;
  }
  return screen_position_x;
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
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->rendered_characters);
  free(row->characters);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.number_of_rows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.number_of_rows - at - 1));
  E.number_of_rows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->characters = realloc(row->characters, row->size + 2);
  memmove(&row->characters[at + 1], &row->characters[at], row->size - at + 1);
  row->size++;
  row->characters[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->characters = realloc(row->characters, row->size + len + 1);
  memcpy(&row->characters[row->size], s, len);
  row->size += len;
  row->characters[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->characters[at], &row->characters[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

void editorInsertChar(int c) {
  if (E.file_position_y == E.number_of_rows) {
    editorInsertRow(E.number_of_rows, "", 0);
  }
  editorRowInsertChar(&E.row[E.file_position_y], E.file_position_x, c);
  E.file_position_x++;
}

void editorInsertNewline() {
  if (E.file_position_x == 0) {
    editorInsertRow(E.file_position_y, "", 0);
  } else {
    erow *row = &E.row[E.file_position_y];
    editorInsertRow(E.file_position_y + 1, &row->characters[E.file_position_x], row->size - E.file_position_x);
    row = &E.row[E.file_position_y];
    row->size = E.file_position_x;
    row->characters[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.file_position_y++;
  E.file_position_x = 0;
}

void editorDelChar() {
  if (E.file_position_y == E.number_of_rows) return;
  if (E.file_position_x == 0 && E.file_position_y == 0) return;

  erow *row = &E.row[E.file_position_y];
  if (E.file_position_x > 0) {
    editorRowDelChar(row, E.file_position_x - 1);
    E.file_position_x--;
  } else {
    E.file_position_x = E.row[E.file_position_y - 1].size;
    editorRowAppendString(&E.row[E.file_position_y - 1], row->characters, row->size);
    editorDelRow(E.file_position_y);
    E.file_position_y--;
  }
}

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.number_of_rows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.number_of_rows; j++) {
    memcpy(p, E.row[j].characters, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}


void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

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
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    editorSaveAs();
    return;
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}


void editorSaveAs() {
  char *filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
  if (filename == NULL) {
    editorSetStatusMessage("Save As aborted");
    return;
  }

  free(E.filename);
  E.filename = filename;

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}



struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}



void editorScroll() {
  // 根据光标的当前位置，计算并更新屏幕的滚动偏移量。
  E.screen_position_x = 0; // 初始化光标的屏幕x坐标。
  if (E.file_position_y < E.number_of_rows) { // 确保光标在文件内。
    // 计算光标的实际屏幕列位置。
    E.screen_position_x = editorRowFilePositionXToScreenPositionX(&E.row[E.file_position_y], E.file_position_x);
  }

  // 垂直滚动检查
  if (E.file_position_y < E.row_offset) {
    E.row_offset = E.file_position_y;
  }
  if (E.file_position_y >= E.row_offset + E.screen_rows) {
    E.row_offset = E.file_position_y - E.screen_rows + 1;
  }

  // 水平滚动检查
  if (E.screen_position_x < E.column_offset) {
    E.column_offset = E.screen_position_x;
  }
  if (E.screen_position_x >= E.column_offset + E.screen_columns) {
    E.column_offset = E.screen_position_x - E.screen_columns + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  for (int y = 0; y < E.screen_rows; y++) {
    int filerow = y + E.row_offset; // 计算当前屏幕行对应的文件行号。
    if (filerow >= E.number_of_rows) {
      // 如果要绘制的行超出了文件的总行数，则显示特殊内容。

      // 如果文件为空，并且当前是屏幕约三分之一处，则显示欢迎信息。
      if (E.number_of_rows == 0 && y == E.screen_rows / 3) {
        char welcome[80];
        // 使用 snprintf 安全地格式化欢迎字符串，并获取其长度。
        int welcomelen = snprintf(welcome, sizeof(welcome), "Texor editor -- version %s", TEXOR_TAG);
        if (welcomelen > E.screen_columns)
          welcomelen = E.screen_columns; // 截断过长的信息。
        int padding = (E.screen_columns - welcomelen) / 2; // 计算居中所需的左边距。
        if (padding) {
          abAppend(ab, "~", 1); // 在行首添加一个 '~'。
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1); // 添加左边距空格。
        abAppend(ab, welcome, welcomelen); // 添加欢迎信息。
      } else {
        // 在其他超出文件内容的行首，只显示一个 '~'。
        abAppend(ab, "~", 1);
      }
    } else {
      // 正常文件行
      int len = E.row[filerow].rendered_size - E.column_offset; // 计算渲染后字符串长度。
      if (len < 0)
        len = 0;
      if (len > E.screen_columns)
        len = E.screen_columns;
      // 从渲染字符串的 `column_offset` 位置开始，追加 `len` 个字符到缓冲区。
      abAppend(ab, &E.row[filerow].rendered_characters[E.column_offset], len);
    }
    abAppend(ab, "\x1b[K", 3); // 清除光标到行尾，确保旧内容被清除。
    abAppend(ab, "\r\n", 2);   // 回车和换行符，移动到下一行行首。
  }
}


void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];

  // 格式化左侧状态信息：文件名 - 行数 (modified)。
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
      E.filename ? E.filename : "[No Name]", E.number_of_rows,
      E.dirty ? "(modified)" : "");
  // 格式化右侧状态信息：当前行/总行数。
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
      E.file_position_y + 1, E.number_of_rows);

  if (len > E.screen_columns)
    len = E.screen_columns; // 截断左侧信息。
  abAppend(ab, status, len);
  // 填充空格，右对齐。
  while (len < E.screen_columns) {
    if (E.screen_columns - len == rlen) {
      abAppend(ab, rstatus, rlen); // 追加右侧信息。
      break;
    } else {
      abAppend(ab, " ", 1); // 填充空格。
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3); //VT100 "关闭所有文本属性" ，恢复正常显示。
  abAppend(ab, "\r\n", 2); // 换行。
}


void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3); // 清除整行。
  int msglen = strlen(E.status_message); // 获取消息长度。
  if (msglen > E.screen_columns)
    msglen = E.screen_columns; // 截断消息。
  // 消息不为空，且距离设置时间不到5秒
  if (msglen && time(NULL) - E.status_message_time < 5)
    abAppend(ab, E.status_message, msglen);
}

void editorRefreshScreen() {
  editorScroll();
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); //  隐藏光标，防止刷新时屏幕闪烁。
  abAppend(&ab, "\x1b[H", 3);    //  光标归位，将光标移动到屏幕左上角(1,1)。

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  // 格式化 "移动光标" 序列，将其移动到正确的编辑位置。
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.file_position_y - E.row_offset) + 1, (E.screen_position_x - E.column_offset) + 1);
  abAppend(&ab, buf, strlen(buf)); // 将该序列追加到缓冲区。
  abAppend(&ab, "\x1b[?25h", 6); // 追加 "显示光标" 序列。

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}


void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  //  vsnprintf 安全地格式化字符串，防止缓冲区溢出。
  vsnprintf(E.status_message, sizeof(E.status_message), fmt, ap);
  va_end(ap);
  E.status_message_time = time(NULL); // 记录消息设置时间
}


char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = INPUT_BUFSIZE;
  char *buf = malloc(bufsize);

  size_t buflen = 0; // 当前输入长度。
  buf[0] = '\0';
  while(1) {

    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey(); // 读取
    // 处理退格/删除
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') { // Esc取消输入。
      editorSetStatusMessage(""); // 清空消息栏
      if (callback)
        callback(buf, c); // 执行可选的回调
      free(buf);
      return NULL; // 返回NULL表示取消。
    } else if (c == '\r') { // 回车键确认
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback)
          callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) { // 普通可打印字符
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c; // 字符添加到缓冲区
      buf[buflen] = '\0';
    }
    if (callback)
      callback(buf, c); // 执行可选的回调
  }
}

// 根据按键 `key` 移动光标。
void editorMoveCursor(int key) {
  // 获取当前光标所在行的指针，如果光标在文件外则为NULL。
  erow *row = (E.file_position_y >= E.number_of_rows) ? NULL : &E.row[E.file_position_y];
  switch (key) {
    case ARROW_LEFT:
      if (E.file_position_x != 0) { // 如果不在行首。
        E.file_position_x--; // 光标左移。
      } else if (E.file_position_y > 0) { // 如果在行首且不在第一行。
        E.file_position_y--; // 移动到上一行。
        E.file_position_x = E.row[E.file_position_y].size; // 移动到上一行的行尾。
      }
      break;
    case ARROW_RIGHT:
      if (row && E.file_position_x < row->size) { // 如果在行内。
        E.file_position_x++; // 光标右移。
      } else if (row && E.file_position_x == row->size) { // 如果在行尾。
        E.file_position_y++; // 移动到下一行。
        E.file_position_x = 0; // 移动到下一行的行首。
      }
      break;
    case ARROW_UP:
      if (E.file_position_y != 0) E.file_position_y--; // 光标上移一行。
      break;
    case ARROW_DOWN:
      if (E.file_position_y < E.number_of_rows) E.file_position_y++; // 光标下移一行。
      break;
  }
  // 修正光标位置：如果光标移动到新一行后，其x坐标超出了新行的长度，则将其x坐标调整为新行的行尾。
  row = (E.file_position_y >= E.number_of_rows) ? NULL : &E.row[E.file_position_y];
  int rowlen = row ? row->size : 0;
  if (E.file_position_x > rowlen) {
    E.file_position_x = rowlen;
  }
}

void editorProcessKeypress() {
  static int quit_times = TEXOR_QUIT_TIMES; // 退出确认的次数
  int c = editorReadKey();

  switch (c) {
    case '\r':
      editorInsertNewline();
      break;

    case CTRL_KEY('q'):
      // 如果文件已修改且退出确认次数未用完。
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4); // 清屏。
      write(STDOUT_FILENO, "\x1b[H", 3);  // 光标归位。
      exit(0);
      break;

    case CTRL_KEY('a'): // Ctrl-A，另存为
      editorSaveAs();
      break;

    case CTRL_KEY('s'): // Ctrl-S，保存
      editorSave();
      break;

    case HOME_KEY:
      E.file_position_x = 0; // 移动光标到行首。
      break;

    case END_KEY:
      if (E.file_position_y < E.number_of_rows)
        E.file_position_x = E.row[E.file_position_y].size; // 移动光标到行尾。
      break;

    case BACKSPACE:
    case CTRL_KEY('h'): // Ctrl-H 在某些终端中等同于退格
    case DEL_KEY:
      // 删除键，等同于“先右移一格，再按退格”。
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar(); // 删除字符
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.file_position_y = E.row_offset; // 移动光标到屏幕顶部。
        } else if (c == PAGE_DOWN) {
          E.file_position_y = E.row_offset + E.screen_rows - 1; // 移动光标到屏幕底部。
          if (E.file_position_y > E.number_of_rows) E.file_position_y = E.number_of_rows;
        }
        // 模拟翻页：连续移动一整屏的行数。
        int times = E.screen_rows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c); // 光标移动
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      editorInsertChar(c); // 视为普通字符插入。
      break;
  }
  // 任何非退出确认的操作，就退出确认计数器。
  quit_times = TEXOR_QUIT_TIMES;
}

void initEditor() {
  E.file_position_x = 0;
  E.file_position_y = 0;
  E.screen_position_x = 0;
  E.row_offset = 0;
  E.column_offset = 0;
  E.number_of_rows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  // 初始化消息栏。
  E.status_message[0] = '\0';
  E.status_message_time = 0;
  // 获取终端窗口大小。
  if (getWindowSize(&E.screen_rows, &E.screen_columns) == -1) die("getWindowSize");
  // 为状态栏和消息栏预留出底部的2行空间。
  E.screen_rows -= 2;
}



int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage(
      "HELP: Ctrl-S = save | Ctrl-A = save as | Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}