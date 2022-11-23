/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define NOTEC_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT, //1001
    ARROW_UP,    //1002
    ARROW_DOWN,  //1003
    DEL_KEY,     //1004
    HOME_KEY,    //1005
    END_KEY,     //1006
    PAGE_UP,     //1007
    PAGE_DOWN    //1008`
};

/*** data ***/

struct editorConfig {
    int cx, cy;//for cursor position
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;
/*** terminal ***/

void die(const char* s) {
    write(STDOUT_FILENO, "\x1b[2J", 4); //clear entire screen
    write(STDOUT_FILENO, "\x1b[H", 3);  //reposition the cursor to the begining

    perror(s);
    exit(1);
}

void disableRawMode()
{
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1){
        die("tcgetattr");
    }

    tcgetattr(STDIN_FILENO, &E.orig_termios);

    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1){
        die("tcsetattr");
    };
}

int editorReadKey() {
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if(nread == -1 && errno != EAGAIN) die("read");
    }
    if(c == '\x1b'){ 
        char seq[3];
        
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if(seq[0] == '[') {
                if(seq[1] >= '0' && seq[1] <= '9') {  //we declared seq to be able to store 3 bytes. If the byte after [ is a digit, we read another byte expecting it to be a ~. Then we test the digit byte to see if it’s a 5 or a 6.
                    if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                    if(seq[2] == '~'){
                        switch (seq[1])
                        {
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
                    case 'A': return ARROW_UP; // up
                    case 'B': return ARROW_DOWN; // down
                    case 'C': return ARROW_RIGHT; // right
                    case 'D': return ARROW_LEFT; // left

                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') { // handling <esc>OH --> HOME_KEY && <esc>OF --> END_KEY
            switch (seq[1]){
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

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while( i < sizeof(buf) - 1){
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[') return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int * cols) {
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorDrawRows(struct abuf *ab) {
    int y;
    for(y=0; y < E.screenrows; y++) {

        if(y==E.screenrows/3){
            char welcome[80];
            int welcomelen = snprintf(welcome,sizeof(welcome),"NoteC editor version -- version %s", NOTEC_VERSION);
            if(welcomelen > E.screencols) welcomelen = E.screencols;
            //centering the welcome message
            
            int padding = (E.screencols - welcomelen)/2;
            if(padding){
                abAppend(ab,"~",1);
                padding--;
            }
            while(padding--) abAppend(ab," ", 1);

            //
            abAppend(ab,welcome,welcomelen);
        }else{
            abAppend(ab,"~",1);
        }

        abAppend(ab, "\x1b[k", 3);//---> refreshing one line at a time
        if(y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);//set mode-->hide the cursor
    // abAppend(&ab, "\x1b[2J" , 4); -->to stop refreshing/clearing entire screen
    abAppend(&ab, "\x1b[H", 3); 

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy+1, E.cx+1);//We add 1 to E.cy and E.cx to convert from 0-indexed values to the 1-indexed values that the terminal uses.
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);//reset mode-->to turn on the cursor again

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key){
    switch(key) {
        case ARROW_LEFT:    if(E.cx != 0){
                                E.cx--;
                            }
                            break;
        
        case ARROW_RIGHT:   if(E.cx != E.screencols-1){
                                E.cx++;
                            }
                            break;
        
        case ARROW_UP:      if(E.cy != 0){
                                E.cy--;
                            }
                            break;
        
        case ARROW_DOWN:    if(E.cy != E.screenrows-1){
                                E.cy++;
                            }
                            break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();
    //mapping of key press
    switch (c) {
        case CTRL_KEY('q'): write(STDOUT_FILENO, "\x1b[2J", 4);// clear screen and reposition cursor to the start after quitting from editor
                            write(STDOUT_FILENO, "\x1b[H", 3);
                            exit(0);
                            break;
        
        case HOME_KEY:      E.cx = 0;
                            break;

        case END_KEY:       E.cx = E.screencols - 1;
                            break;

        case PAGE_UP:
        case PAGE_DOWN:     {   // We create a code block with that pair of braces so that we’re allowed to declare the times variable. (You can’t declare variables directly inside a switch statement.) 
                                int times = E.screenrows;
                                while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN); // repeat arrow_up/arrow_down fn (E.screenrows) times
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

/*** init ***/

void intiEditor() {
    //initialise cursors position
    E.cx = 0;
    E.cy = 0;

    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    intiEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    };

    return 0;
}