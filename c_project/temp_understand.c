/*** 包含的头文件 ***/

// 宏定义，用于启用一些非标准的、但广泛使用的POSIX和GNU C库扩展功能。
// 它们确保了像 getline() 这样的函数能够被正确声明和使用。
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>      // 包含字符处理函数的声明，例如 iscntrl() 用于检查字符是否是控制字符。
#include <errno.h>      // 包含错误码 'errno' 的定义，用于获取系统调用失败时的具体错误信息。
#include <fcntl.h>      // 包含文件控制函数的声明，例如 open() 用于打开文件。
#include <stdio.h>      // 包含标准输入输出函数的声明，例如 perror(), sscanf(), FILE, fopen(), fclose()。
#include <stdarg.h>     // 包含处理可变参数列表的宏，例如 va_list, va_start, va_end，用于实现像 printf 一样的函数。
#include <stdlib.h>     // 包含通用工具函数的声明，例如 exit(), malloc(), realloc(), free(), atexit()。
#include <string.h>     // 包含字符串处理函数的声明，例如 memcpy(), strlen(), strerror(), strdup()。
#include <sys/ioctl.h>  // 包含 I/O 控制函数 ioctl() 的声明，用于与设备驱动交互，如此处的获取终端窗口大小。
#include <sys/types.h>  // 包含基本系统数据类型的定义，例如 size_t, ssize_t。
#include <termios.h>    // 包含终端 I/O 接口的定义，例如 struct termios 和 tcgetattr(), tcsetattr() 函数。
#include <time.h>       // 包含时间相关函数的声明，例如 time() 用于获取当前时间戳。
#include <unistd.h>     // 包含 POSIX 操作系统 API 函数的声明，例如 read(), write(), close(), ftruncate()。

#include "terminal.h"   // 包含我们自己分离出去的终端控制模块的头文件。

/*** 宏定义 ***/

#define TEXOR_VERSION "0.0.1"      // 定义编辑器的版本号字符串常量。
#define TEXOR_TAB_STOP 8           // 定义一个Tab键在屏幕上代表的空格数。
#define TEXOR_QUIT_TIMES 2         // 定义在文件未保存时，需要按几次Ctrl-Q才能强制退出的次数。

// 一个宏，用于计算Ctrl键与字母键组合后的ASCII码。
// 它的原理是利用了大多数终端中Ctrl组合键的值等于对应字母ASCII码的低5位这一特性。
// `& 0x1f` 的结果范围是 0-31。
#define CTRL_KEY(k) ((k) & 0x1f)

/*** 数据结构定义 ***/

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
  int rendered_size;        // 行内容渲染到屏幕上后所占的字符宽度（例如，Tab会被转换成多个空格）。
  char *characters;         // 指向存储该行原始字符内容的动态分配内存的指针。
  char *rendered_characters;// 指向存储该行渲染后字符内容的动态分配内存的指针。
} erow;

// 定义了包含编辑器所有状态和配置的全局结构体。
struct editorConfig {
  int file_position_x, file_position_y; // 光标在文件内容中的逻辑坐标 (x: 列索引, y: 行索引)。
  int screen_position_x;    // 光标在屏幕上经过渲染（如Tab转换）后的实际列坐标。
  int row_offset;           // 垂直滚动偏移量。表示屏幕顶部显示的是文件的第几行。
  int column_offset;        // 水平滚动偏移量。表示屏幕最左侧显示的是渲染后内容的第几列。
  int screen_rows;          // 终端窗口可用的行数。
  int screen_columns;       // 终端窗口可用的列数。
  int number_of_rows;       // 文件当前的总行数。
  erow *row;                // 一个指向动态数组的指针，该数组的每个元素都是一个 erow 结构体，代表文件的一行。
  int dirty;                // “脏”标志。如果大于0，表示文件内容自上次保存后已被修改。
  char *filename;           // 指向当前打开文件名的字符串指针。
  char status_message[80];  // 用于在状态栏显示临时消息的字符数组。
  time_t status_message_time; // 记录状态栏消息显示时的时间戳，用于实现消息的自动消失。
  // struct termios orig_termios; // 原始终端设置已被移至 terminal.c 模块内部管理。
};
// 声明一个全局的 editorConfig 实例，命名为 E。程序的所有部分都通过这个全局变量来访问和修改编辑器状态。
struct editorConfig E;

/*** 函数原型声明 ***/

void editorSetStatusMessage(const char *fmt, ...); // 设置状态栏消息的函数原型，支持可变参数。
void editorRefreshScreen();                        // 刷新整个编辑器屏幕的函数原型。
char *editorPrompt(char *prompt, void (*callback)(char *, int)); // 显示用户输入提示框并获取输入的函数原型。
void editorSaveAs();                               // 实现“另存为”功能的函数原型。

/*** 终端交互相关函数 ***/


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

// 获取光标的当前位置。
int getCursorPosition(int *rows, int *cols) {
  char buf[32]; // 用于存储终端返回的位置报告。
  unsigned int i = 0; // 缓冲区索引。
  // 向标准输出写入DSR (Device Status Report) 控制序列 `\x1b[6n`，请求终端报告光标位置。
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  // 循环读取终端的响应，直到遇到报告结束符 'R'。
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0'; // 在字符串末尾添加空终止符。
  // 检查响应格式是否正确，应为 `\x1b[<rows>;<cols>R`。
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  // 使用 sscanf 从字符串中解析出行号和列号。
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0; // 成功返回0。
}

// 获取终端窗口的大小（行数和列数）。
int getWindowSize(int *rows, int *cols) {
  struct winsize ws; // 用于存储窗口大小的结构体。
  // 尝试使用 ioctl 系统调用和 TIOCGWINSZ 请求来直接获取窗口大小，这是最可靠的方法。
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // 如果 ioctl 失败，采用备用方案：将光标移动到屏幕右下角（一个很大的坐标），然后查询其位置。
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols); // 调用 getCursorPosition 获取实际位置，即窗口大小。
  } else {
    // 如果 ioctl 成功，直接赋值。
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** 行操作相关函数 ***/

// 将文件中字符的逻辑列位置(file_position_x)转换为其在屏幕上渲染后的实际列位置(screen_position_x)。
int editorRowFilePositionXToScreenPositionX(erow *row, int file_position_x) {
  int screen_position_x = 0;
  int j;
  // 遍历到目标逻辑位置前的所有字符。
  for (int j = 0; j < file_position_x; j++) {
    if (row->characters[j] == '\t')
      // 如果遇到Tab，需要增加的宽度是到下一个Tab Stop位置所需的空格数。
      screen_position_x += (TEXOR_TAB_STOP - 1) - (screen_position_x % TEXOR_TAB_STOP);
    screen_position_x++; // 每个字符（包括Tab转换后的第一个空格）自身占一列。
  }
  return screen_position_x; // 返回计算出的屏幕列位置。
}

// 更新一行的渲染版本（`rendered_characters`）。
void editorUpdateRow(erow *row) {
  int tabs = 0; // 统计行中Tab的数量。
  int j; // 循环变量。
  for (j = 0; j < row->size; j++)
    if (row->characters[j] == '\t') tabs++;

  free(row->rendered_characters); // 释放之前分配的渲染字符串内存。
  // 为渲染字符串分配新的内存。大小为原字符数 + 所有Tab转换为空格后额外增加的字符数 + 1个字节的结束符。
  row->rendered_characters = malloc(row->size + tabs * (TEXOR_TAB_STOP - 1) + 1);

  int index = 0; // 渲染字符串的当前索引。
  for (j = 0; j < row->size; j++) {
    if (row->characters[j] == '\t') {
      // 如果是Tab，先添加一个空格。
      row->rendered_characters[index++] = ' ';
      // 然后继续添加空格，直到当前索引是 TAB_STOP 的整数倍。
      while (index % TEXOR_TAB_STOP != 0) row->rendered_characters[index++] = ' ';
    } else {
      // 如果是普通字符，直接复制。
      row->rendered_characters[index++] = row->characters[j];
    }
  }
  row->rendered_characters[index] = '\0'; // 在渲染字符串末尾添加空终止符。
  row->rendered_size = index;             // 更新渲染后的大小。
}

// 在指定索引 `at` 处插入一个新行。
void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.number_of_rows) return; // 检查插入位置是否有效。

  // 重新分配行数组 `E.row` 的内存，以容纳一个新行。
  E.row = realloc(E.row, sizeof(erow) * (E.number_of_rows + 1));
  // 使用 memmove 将从 `at` 位置开始的所有行向后移动一个位置，为新行腾出空间。
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.number_of_rows - at));

  // 初始化新插入的行 `E.row[at]` 的各个字段。
  E.row[at].size = len;
  E.row[at].characters = malloc(len + 1); // 为新行的内容分配内存。
  memcpy(E.row[at].characters, s, len); // 复制行内容。
  E.row[at].characters[len] = '\0'; // 添加字符串结束符。

  E.row[at].rendered_size = 0; // 初始化渲染大小为0。
  E.row[at].rendered_characters = NULL; // 初始化渲染内容指针为NULL。
  editorUpdateRow(&E.row[at]); // 调用函数更新新行的渲染版本。

  E.number_of_rows++; // 总行数加一。
  E.dirty++;          // 将文件标记为已修改。
}

// 释放一个 erow 结构体所占用的动态内存。
void editorFreeRow(erow *row) {
  free(row->rendered_characters); // 释放渲染字符串的内存。
  free(row->characters);         // 释放原始字符串的内存。
}

// 删除指定索引 `at` 处的行。
void editorDelRow(int at) {
  if (at < 0 || at >= E.number_of_rows) return; // 检查索引是否有效。
  editorFreeRow(&E.row[at]); // 释放要删除的行的内存。
  // 使用 memmove 将 `at` 之后的所有行向前移动一个位置，覆盖掉被删除的行。
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.number_of_rows - at - 1));
  E.number_of_rows--; // 总行数减一。
  E.dirty++;          // 将文件标记为已修改。
}

// 在行的指定列 `at` 处插入一个字符 `c`。
void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size; // 确保插入位置在 [0, row->size] 范围内。
  // 为行内容重新分配内存，增加2个字节（1个为新字符，1个为已有的结束符）。
  row->characters = realloc(row->characters, row->size + 2);
  // 使用 memmove 将从 `at` 位置开始的所有字符向后移动一个位置。
  memmove(&row->characters[at + 1], &row->characters[at], row->size - at + 1);
  row->size++; // 行的大小加一。
  row->characters[at] = c; // 在 `at` 位置放入新字符。
  editorUpdateRow(row); // 更新该行的渲染版本。
  E.dirty++;            // 标记为已修改。
}

// 在行尾追加一个字符串 `s`。
void editorRowAppendString(erow *row, char *s, size_t len) {
  // 为行内容重新分配内存，以容纳追加的字符串。
  row->characters = realloc(row->characters, row->size + len + 1);
  // 使用 memcpy 将字符串s复制到原字符串的末尾。
  memcpy(&row->characters[row->size], s, len);
  row->size += len; // 更新行的大小。
  row->characters[row->size] = '\0'; // 在新末尾添加字符串结束符。
  editorUpdateRow(row); // 更新行的渲染版本。
  E.dirty++;            // 标记为已修改。
}

// 删除行中指定列 `at` 处的一个字符。
void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return; // 检查索引是否有效。
  // 使用 memmove 将 `at` 之后的字符向前移动一个位置，覆盖掉被删除的字符。
  memmove(&row->characters[at], &row->characters[at + 1], row->size - at);
  row->size--; // 行的大小减一。
  editorUpdateRow(row); // 更新行的渲染版本。
  E.dirty++;            // 标记为已修改。
}

/*** 编辑核心操作 ***/

// 在光标当前位置插入一个字符。
void editorInsertChar(int c) {
  // 如果光标位于文件末尾（即在最后一行之后），需要先添加一个新行。
  if (E.file_position_y == E.number_of_rows) {
    editorInsertRow(E.number_of_rows, "", 0);
  }
  // 在当前行的光标位置插入字符。
  editorRowInsertChar(&E.row[E.file_position_y], E.file_position_x, c);
  E.file_position_x++; // 光标向右移动一格。
}

// 插入一个新行（处理回车键）。
void editorInsertNewline() {
  if (E.file_position_x == 0) {
    // 光标在行首，直接插入新行。
    editorInsertRow(E.file_position_y, "", 0);
  } else {
    // 如果光标在行中，将当前行从光标处分割成两行。
    erow *row = &E.row[E.file_position_y]; // 获取当前行的指针。
    // 在下一行插入一个新行，其内容为原行光标位置及之后的部分。
    editorInsertRow(E.file_position_y + 1, &row->characters[E.file_position_x], row->size - E.file_position_x);
    // realloc 可能导致 E.row 的地址改变，所以需要重新获取指针。
    row = &E.row[E.file_position_y];
    // 截断原行。
    row->size = E.file_position_x;
    row->characters[row->size] = '\0';
    editorUpdateRow(row); // 更新被截断行的渲染版本。
  }
  E.file_position_y++; // 光标下移到新行。
  E.file_position_x = 0; // 光标移动到新行的行首。
}

// 删除光标前的字符（处理退格键）。
void editorDelChar() {
  if (E.file_position_y == E.number_of_rows) return; // 如果光标在文件底部之外，不执行任何操作。
  if (E.file_position_x == 0 && E.file_position_y == 0) return; // 如果在文件的最开始，不执行任何操作。

  erow *row = &E.row[E.file_position_y]; // 获取当前行的指针。
  if (E.file_position_x > 0) {
    // 如果光标不在行首，直接删除光标前的一个字符。
    editorRowDelChar(row, E.file_position_x - 1);
    E.file_position_x--; // 光标左移。
  } else {
    // 如果光标在行首，则需要将当前行与上一行合并。
    E.file_position_x = E.row[E.file_position_y - 1].size; // 将光标的x坐标移动到上一行的末尾。
    // 将当前行的内容追加到上一行。
    editorRowAppendString(&E.row[E.file_position_y - 1], row->characters, row->size);
    editorDelRow(E.file_position_y); // 删除当前行。
    E.file_position_y--; // 光标上移一行。
  }
}

/*** 文件 I/O 操作 ***/

// 将内存中所有的行数据转换成一个单一的、用换行符分隔的字符串。
char *editorRowsToString(int *buflen) {
  int totlen = 0; // 用于计算总长度。
  int j; // 循环变量。
  // 遍历所有行，累加每行的长度和换行符（+1）。
  for (j = 0; j < E.number_of_rows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen; // 通过指针返回总长度。

  char *buf = malloc(totlen); // 分配足够大的内存。
  char *p = buf; // 创建一个指针 `p` 用于在缓冲区中移动。
  // 再次遍历所有行。
  for (j = 0; j < E.number_of_rows; j++) {
    memcpy(p, E.row[j].characters, E.row[j].size); // 将行内容复制到缓冲区。
    p += E.row[j].size; // 移动指针 `p` 到刚复制内容的末尾。
    *p = '\n'; // 添加换行符。
    p++; // 移动指针 `p` 越过换行符。
  }

  return buf; // 返回包含所有文件内容的字符串缓冲区。
}


void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename); //  strdup 复制并保存新的文件名，strdup动态分配内存。

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL; // 行缓冲区指针。
  size_t linecap = 0; // 行缓冲区的容量。
  ssize_t linelen; // 读取到的行长度。
  // 使用 getline 循环逐行读取文件，它会自动处理内存分配。
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    // 循环去除行尾可能存在的换行符 '\n' 或回车符 '\r'。
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    // 将处理过的行插入到编辑器中。
    editorInsertRow(E.number_of_rows, line, linelen);
  }

  free(line);
  fclose(fp);
  E.dirty = 0;
}

// 保存当前编辑器的内容到文件。
void editorSave() {
  if (E.filename == NULL) {
    // 如果当前文件没有名字（是新文件），则调用“另存为”逻辑。
    editorSaveAs();
    return;
  }

  int len; // 用于存储文件内容的总长度。
  char *buf = editorRowsToString(&len); // 将编辑器内容转换为字符串。

  // 使用 open 系统调用打开文件。O_RDWR: 读写模式, O_CREAT: 如果文件不存在则创建。
  // 0644 是文件权限：所有者可读写，同组用户和其他用户只可读。
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    // 使用 ftruncate 将文件大小调整为新内容的长度，可以处理文件变短的情况。
    if (ftruncate(fd, len) != -1) {
      // 使用 write 将缓冲区内容写入文件。
      if (write(fd, buf, len) == len) {
        close(fd); // 关闭文件。
        free(buf); // 释放缓冲区内存。
        E.dirty = 0; // 保存成功，清除脏标志。
        editorSetStatusMessage("%d bytes written to disk", len); // 显示成功消息。
        return; // 成功保存后直接返回。
      }
    }
    close(fd); // 如果有任何步骤失败，也要关闭文件。
  }

  free(buf); // 即使保存失败，也要释放缓冲区内存。
  // 使用 strerror(errno) 获取I/O错误的具体文本描述。
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

// 实现“另存为”功能。
void editorSaveAs() {
  // 调用 editorPrompt 提示用户输入新文件名。
  char *filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
  if (filename == NULL) { // 如果用户按ESC取消。
    editorSetStatusMessage("Save As aborted");
    return;
  }

  // 更新编辑器的文件名状态。
  free(E.filename); // 释放旧的文件名。
  E.filename = filename; // 将 editorPrompt 返回的新文件名赋给 E.filename。

  // 后续的保存逻辑与 editorSave() 完全相同。
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



// 一个动态字符串缓冲区结构体，用于在刷新屏幕时高效地构建输出字符串，避免多次调用 write。
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
        int welcomelen = snprintf(welcome, sizeof(welcome), "Texor editor -- version %s", TEXOR_VERSION);
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



// 显示一个输入提示框，并等待用户输入字符串。
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128; // 初始缓冲区大小。
  char *buf = malloc(bufsize); // 为用户输入分配内存。

  size_t buflen = 0; // 当前输入长度。
  buf[0] = '\0'; // 初始化为空字符串。
  while(1) {
    // 在消息栏显示提示信息，并动态显示用户已输入的内容。
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen(); // 刷新屏幕以显示提示。

    int c = editorReadKey(); // 读取一个按键。
    // 处理退格/删除键。
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') { // 如果是Esc键，取消输入。
      editorSetStatusMessage(""); // 清空消息栏。
      if (callback) callback(buf, c); // 执行可选的回调。
      free(buf); // 释放缓冲区。
      return NULL; // 返回NULL表示取消。
    } else if (c == '\r') { // 如果是回车键，确认输入。
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf; // 返回用户输入的字符串。
      }
    } else if (!iscntrl(c) && c < 128) { // 如果是普通可打印字符。
      // 如果缓冲区满了，则将其大小加倍。
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c; // 将字符添加到缓冲区。
      buf[buflen] = '\0'; // 更新字符串结束符。
    }
    if (callback) callback(buf, c); // 执行可选的回调。
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
    case '\r': // 回车键。
      editorInsertNewline();
      break;

    case CTRL_KEY('q'): // Ctrl-Q，退出命令。
      // 如果文件已修改且退出确认次数未用完。
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--; // 确认次数减一。
        return; // 直接返回，等待下一次按键。
      }
      // 确认退出或文件未修改，则清理屏幕并退出程序。
      write(STDOUT_FILENO, "\x1b[2J", 4); // 清屏。
      write(STDOUT_FILENO, "\x1b[H", 3);  // 光标归位。
      exit(0); // 正常退出。
      break;

    case CTRL_KEY('a'): // Ctrl-A，另存为命令。
      editorSaveAs();
      break;

    case CTRL_KEY('s'): // Ctrl-S，保存命令。
      editorSave();
      break;

    case HOME_KEY: // Home键。
      E.file_position_x = 0; // 移动光标到行首。
      break;

    case END_KEY: // End键。
      if (E.file_position_y < E.number_of_rows)
        E.file_position_x = E.row[E.file_position_y].size; // 移动光标到行尾。
      break;

    case BACKSPACE:     // 退格键。
    case CTRL_KEY('h'): // Ctrl-H (在某些终端中等同于退格)。
    case DEL_KEY:       // 删除键。
      // 如果是删除键，其行为等同于“先右移一格，再按退格”。
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar(); // 调用删除字符的函数。
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
      editorMoveCursor(c); // 调用光标移动函数。
      break;

    case CTRL_KEY('l'): // Ctrl-L: 传统上用于刷新屏幕，这里不做任何事，让主循环自然刷新即可。
    case '\x1b':        // Escape键：通常用于取消操作，这里直接忽略。
      break;

    default: // 所有其他未被特殊处理的按键。
      editorInsertChar(c); // 视为普通字符插入。
      break;
  }
  // 只要用户执行了任何非退出确认的操作，就重置退出确认计数器。
  quit_times = TEXOR_QUIT_TIMES;
}

/*** 初始化 ***/

// 初始化编辑器状态。
void initEditor() {
  // 初始化光标和滚动位置为0。
  E.file_position_x = 0;
  E.file_position_y = 0;
  E.screen_position_x = 0;
  E.row_offset = 0;
  E.column_offset = 0;
  // 初始化文件信息。
  E.number_of_rows = 0;
  E.row = NULL; // 初始化行数组指针为空。
  E.dirty = 0;  // 初始为未修改状态。
  E.filename = NULL; // 初始没有文件名。
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