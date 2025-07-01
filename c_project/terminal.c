#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "terminal.h"


static struct termios orig_termios_static;

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_static) == -1)
        die("tcsetattr");
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios_static) == -1) die("tcgetattr");

    atexit(disableRawMode);

    struct termios raw = orig_termios_static;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}
