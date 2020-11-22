#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>

struct termios orig_termios;

void die(const char* s){
    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1){
        die("tcsetattr");
    }
}

void enableRawMode() {

    if(tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        die("tcgetattr");
    };
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    // 关闭echo和canonical mode。可以按
    // 字节输入。
    // ISIG 表示关闭 ctrl-c 和 ctrl-z信号
    // IXON 表示关闭 ctrl-s 和 ctrl-q
    // IEXTEN 关闭 ctrl-v
    // ICRNL 关闭 回车到换行映射
    raw.c_iflag &= ~(BRKINT|ICRNL|IXON|INPCK|ISTRIP);
    // \n 字符转为 \r\n
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    // read 之前等待的最小字节数量
    raw.c_cc[VMIN] = 0;
    // read 之前最小等待时间
    raw.c_cc[VTIME] = 1;
    
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    };
}

int main(){

    enableRawMode();
    while (1) {
        char c = '\0';

        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN){
            die("read");
        }
        // contorl character are nonprintable（ASCII 0~31）。
        if(iscntrl(c)) {
            printf("%d \r\n", c);
        } else {
            printf("%d ('%c')\r\n", c, c);
        }
        if (c == 'q') break;
    };
    
    return 0;
}