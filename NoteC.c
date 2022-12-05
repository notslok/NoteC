/*** includes ***/

//Feature Test Macros:
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE //to enable "getline()" on GNU systems .... (https://stackoverflow.com/questions/59014090/warning-implicit-declaration-of-function-getline)
//-------------------

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
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

typedef struct erow
{
    int size;
    char *chars;
} erow;

struct editorConfig 
{
    int cx, cy; // for cursor position
    int rowoff; // row offset...for vertical scrolling
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row; // to store multiple line
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
    atexit(disableRawMode);

    // tcgetattr(STDIN_FILENO, &E.orig_termios);


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
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len){
    E.row = realloc(E.row, sizeof(erow)*(E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename) // editorOpen() will eventually be for opening and reading a file from disk, so we put it in a new /*** file i/o ***/ section.
{          
    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen; //ssize_t can represent -1 signal unlike size_t
    linelen = getline(&line, &linecap, fp);

    if(linelen != -1) {
        while( (linelen = getline(&line, &linecap, fp)) != -1 ) { // keep reading line from the file till EOF is encountered(hence returning -1 through getline())
            while(linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) //reducing linelen till escape charaters are out of its range
                linelen--;

            editorAppendRow(line, linelen);
        }
    }  

    free(line);
    fclose(fp);  
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

void editorScroll() {
    /* 
    checks if the cursor is above the visible window, and if so, 
    scrolls up to where the cursor is.
    */
    if(E.cy < E.rowoff){
        E.rowoff = E.cy;
    }
    /*
    checks if the cursor is past the bottom of the visible window,
    */
    if(E.cy >= E.rowoff + E.screenrows){    //E.rowoff refers to what’s at the top of the screen, and we have to get E.screenrows involved to talk about what’s at the bottom of the screen.
        E.rowoff = E.cy - E.screenrows + 1; 
    }

    /* horizontal scrolling logic */
    if(E.cx < E.coloff){
        E.coloff = E.cx;
    }
    if(E.cx >= E.coloff + E.screencols) {
        E.coloff = E.cx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int y;
    for(y=0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if(filerow >= E.numrows){ 
            if(E.numrows == 0 && y == E.screenrows / 3){ 
                    char welcome[80];
                    int welcomelen = snprintf(welcome,sizeof(welcome),"NoteC editor version -- version %s", NOTEC_VERSION);
                    if(welcomelen > E.screencols) welcomelen = E.screencols;
                    
                    //centering the welcome message
                    int padding = (E.screencols - welcomelen)/2;
                    if(padding){
                        abAppend(ab, "~", 1);
                        padding--;
                    }
                    while(padding--) abAppend(ab," ", 1);
                    //
                    abAppend(ab, welcome, welcomelen); 
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].size - E.coloff;// getting size of each line in E.row[] buffer
            
            if(len < 0) 
            {
                len = 0;
            }
            
            if(len > E.screencols) 
            {
                len = E.screencols;
            }

            abAppend(ab, &E.row[filerow].chars[E.coloff], len);//extract each line from "E.row[]" buffer and pushing it in output buffer "ab"
        }

        abAppend(ab, "\x1b[k", 3);//---> refreshing one line at a time
        if(y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);//set mode-->hide the cursor
    // abAppend(&ab, "\x1b[2J" , 4); -->to stop refreshing/clearing entire screen
    abAppend(&ab, "\x1b[H", 3); 

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 1);//We add 1 to E.cy and E.cx to convert from 0-indexed values to the 1-indexed values that the terminal uses.
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);//reset mode-->to turn on the cursor again

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key){
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch(key) {
        case ARROW_LEFT:    if(E.cx != 0){
                                E.cx--;
                            } else if (E.cy > 0) { // allowing the user to press ← at the beginning of the line to move to the end of the previous line.
                                E.cy--; // set cursor to prev. line
                                E.cx = E.row[E.cy].size; // move cursor to prev. line's ending
                            }
                            break;
        
        case ARROW_RIGHT:   if(row && E.cx < row->size){
                                E.cx++;
                            } else if (row && E.cx == row->size){ // ...if cursor is at a valid row AND cursor is pointing at a valid column
                                E.cy++; // move cursor to next line
                                E.cx = 0; // set the cursor to the starting of the next line
                            } // <--------------- ckpt. step 79
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
    
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if(E.cx > rowlen){
        E.cx = rowlen; //by moving the cursor to the end of a long line, then moving it down to the next line, which is shorter. The E.cx value won’t change, and the cursor will be off to the right of the end of the line it’s now on.
    //!!! improvement : vs code like vertical scrolling !!!
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

void initEditor() {
    //initialise cursors position
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0; // Initializing it to 0, which means we’ll be scrolled to the top of the file by default.
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;

    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if(argc >= 2){ // i.e. execute only if filename is also passed in terminal command
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    };

    return 0;
}