#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define _DEFALUT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#define YOUR_EDITOR_VERSION "0.0.1"
// ctrl 组合键时，其编码会将对应按键第5，6位设置为0
#define CTRL_KEY(k) ((k)&0x1f)

#define LINE_HEAD "~"

enum editorKey{
    ARROW_LEFT = 1000,
    ARROW_RIGHT = 1001,
    ARROW_UP = 1002,
    ARROW_DOWN = 1003,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

/*** data ***/

typedef struct erow {
    int size;
    char *chars;
} erow;

struct editorConfig {
    // 当前光标位置
    int cx, cy;
    int rowoff;
    int screenrows;
    int screencols;

    int numrows;
    erow *row;
    struct termios orig_termios;
};

struct editorConfig E;

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {

    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    };
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    // 关闭echo和canonical mode。可以按
    // 字节输入。
    // ISIG 表示关闭 ctrl-c 和 ctrl-z信号
    // IXON 表示关闭 ctrl-s 和 ctrl-q
    // IEXTEN 关闭 ctrl-v
    // ICRNL 关闭 回车到换行映射
    raw.c_iflag &= ~(BRKINT | ICRNL | IXON | INPCK | ISTRIP);
    // \n 字符转为 \r\n
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    // read 之前等待的最小字节数量
    raw.c_cc[VMIN] = 0;
    // read 之前最小等待时间
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    };
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }
    // 映射上下左右方向键到wsad，
    // 方向键以转移字符开头，后面为 ABCD 字母
    if (c == '\x1b') {
        char seq[3];

        if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if(seq[0] == '[') {
            // page up, page down. [5~, [6~
            // del [3~]
            if(seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch(seq[1]) {
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '1':
                        case '7': return HOME_KEY;
                        case '4':
                        case '8': return END_KEY;
                        case '3': return DEL_KEY;
                    }
                }
            }else {
                switch(seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch(seq[1]) {
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

    // 写入 n 命令获取屏幕信息，参数6表示获取光标信息。
    // 后面通过 read 来读取返回的信息
    // 返回信息如：\x1b[30;100R
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDOUT_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    // ioctl 获取屏幕窗口尺寸失败时，一种策略是将光标移动到屏幕右下角
    // 间接获取屏幕窗口尺寸
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0&&(line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen --;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
}

/*** append buffer ***/

/**
 *
 * 构建缓冲区，而不是直接write
 */
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0};

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/*** output ***/

void printWelcome(struct abuf *ab, char *s, int len){
    
    int padding = (E.screencols - len) / 2;
    if (padding) {
        abAppend(ab, LINE_HEAD, 1);
        padding--;
    }
    while (padding--) {
        abAppend(ab, " ", 1);
    }
    abAppend(ab, s, len);
}

void editorScroll() {
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
}

/**
 *
 * 绘制 "~"
 */
void editorDrawRows(struct abuf *ab) {
    int y;
    int welcomep = E.screenrows / 3;
    int authorp = welcomep + 2;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0) {
                if (y == welcomep) {
                    char welcome[80];
                    int welcomelen =
                        snprintf(welcome, sizeof(welcome), "Your editor -- version %s",
                                    YOUR_EDITOR_VERSION);
                    if (welcomelen > E.screencols) {
                        welcomelen = E.screencols;
                    }
                    printWelcome(ab, welcome, welcomelen);
                } else if(y == authorp){
                    char author[80];
                    int authorlen =
                        snprintf(author, sizeof(author), "by xiaofei");
                    if (authorlen > E.screencols) {
                        authorlen = E.screencols;
                    }
                    printWelcome(ab, author, authorlen);
                } else {
                    abAppend(ab, LINE_HEAD, 1);
                }
            } else {
                abAppend(ab, LINE_HEAD, 1);
            }
            
        } else {
            int len = E.row[filerow].size;
            if (len > E.screencols) {
                len = E.screencols;
            }
            abAppend(ab, E.row[filerow].chars, len);
        }
        // K 表示清除当前行
        // 和J的作用类似
        // 默认0，清除光标到行尾内容
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // 隐藏光标
    abAppend(&ab, "\x1b[?25l", 6);
    // \x1b(27) is escape。
    // 0 表示清除从光标到屏幕结尾；
    // 1 表示清除从屏幕开始到光标位置；
    // 2 表示清除整个屏幕。清除完之后，
    // 光标在屏幕末尾位置。
    // 默认 0
    // abAppend(&ab, "\x1b[2J", 4);
    abAppend(&ab, "\x1b[H", 3);
    // 每次清空屏幕之后，重新绘制 ‘~’
    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff), E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    // 展示光标
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key) {
    switch (key) {
    case ARROW_LEFT:
        if(E.cx != 0)
            E.cx--;
        break;
    case ARROW_RIGHT:
        if(E.cx != E.screencols - 1)
            E.cx++;
        break;
    case ARROW_UP:
        if(E.cy != 0)
            E.cy--;
        break;
    case ARROW_DOWN:
        if(E.cy < E.numrows)
            E.cy++;
        break;
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
    case PAGE_UP:
    case PAGE_DOWN: {
            int times = E.screenrows;
            while(times--) {
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        }
        break;
    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        E.cx = E.screencols - 1;
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    }
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowoff = 0;
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
}

int main(int argc, char *argv[]) {

    enableRawMode();
    initEditor();
    if (argc >= 2){
        editorOpen(argv[1]);
    }
    while (1) {
        struct abuf ab = ABUF_INIT;
        editorRefreshScreen();
        editorProcessKeypress();
        editorDrawRows(&ab);
        // 重新设定光标位置到左上角
        write(STDOUT_FILENO, "\x1b[H", 3);
    };

    return 0;
}