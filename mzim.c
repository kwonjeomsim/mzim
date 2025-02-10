#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define CBUF_INIT {0, NULL}

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    HOME_KEY,
    END_KEY,
    DELETE_KEY,
    PAGE_UP,
    PAGE_DOWN,
    ENTER_KEY,
    TAB_KEY,
    BACKSPACE
};

struct termios orig_termios;

typedef struct srow {
    int row_index;
    int len;
    char *buf;
} srow;

typedef struct erow {
    int dirty;
    int len;
    char *buf;
} erow;

struct editorInfo {
    char *filename;
    int cursor_x, cursor_y, cursor_y_offset;
    int screen_row, screen_col;
    int numrows;
    int numsrows;
    erow *rows;
    srow *srows;
};

static struct editorInfo info;

/*
 * This structure stores all text data.
 * By updating this structure, you can update text on screen.
 */
struct cbuf {
    int len;
    char *buf;
};

void die(const char *s)
{
    perror(s);
    exit(1);
}

void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int addToCbuf(struct cbuf *cb, char *buf, int len)
{
    char *newbuf = realloc(cb->buf, cb->len + len);
    if (newbuf == NULL) {
        return -1;
    }

    memcpy(newbuf + cb->len, buf, len);
    cb->len += len;
    cb->buf = newbuf;

    return len;
}

void addRowToSrows(int row_index, int add_index, char *buf, int len)
{
    srow newsrow;
    newsrow.row_index = row_index;
    newsrow.buf = buf;
    newsrow.len = len;

    srow *newsrows = realloc(info.srows, (info.numsrows + 1) * sizeof(srow));

    /*
    if (add_index != info.numsrows) {
        memcpy(newsrows + add_index + 2, newsrows + add_index + 1, (info.numsrows - (add_index + 1)) * sizeof(srow));
    }
    */
    newsrows[add_index] = newsrow;

    info.srows = newsrows;
    info.numsrows += 1;
}

int findSplitPoint(int rowlen, char *buf)
{
    int split_point = info.screen_col;
    for (int j = rowlen - 1; j >= 0; j--) {
        if (buf[j] == ' ' && j < info.screen_col && j > info.screen_col / 2) {
            split_point = j + 1;
            break;
        }
    }

    return split_point;
}

void updateSrows()
{
    info.numsrows = 0;
    info.srows = NULL;

    for (int i = 0; i < info.numrows; i++) {
        erow currow = info.rows[i];

        if (currow.len >= info.screen_col) {
            char *tmpbuf = (char *) malloc(currow.len);
            memcpy(tmpbuf, currow.buf, currow.len);

            int split_point;
            int rowlen = currow.len;

            while (rowlen >= info.screen_col) {
                split_point = findSplitPoint(rowlen, tmpbuf);

                int newlen = split_point;
                char *newbuf = (char *) malloc(newlen);
                memcpy(newbuf, tmpbuf, newlen);
                addRowToSrows(i, info.numsrows, newbuf, newlen);

                rowlen -= newlen;
                tmpbuf += newlen;
            }

            if (rowlen >= 0) {
                addRowToSrows(i, info.numsrows, tmpbuf, rowlen);
            }
        }
        else {
            addRowToSrows(i, info.numsrows, currow.buf, currow.len);
        }
    }

    // This part will fill the blank line with ~.
    int diff = info.screen_row - info.numsrows;
    for (int j = 0; j < diff; j++) {
        addRowToSrows(-1, info.numsrows, "~", 1);
    }
}

int getSrowLen(int currow_index)
{
    int result = 0;
    for (int i = info.cursor_y - 2; i >= 0; i--) {
        if (info.srows[i].row_index != currow_index) {
            break;
        }
        result += info.srows[i].len;
    }

    return result;
}

/*
 * This function add input to screen in the cursor's position.
 */
int addCharacter(char *buf, int len)
{
    srow cursrow = info.srows[info.cursor_y + info.cursor_y_offset- 1];
    int row_index = cursrow.row_index;
    erow *currow = &info.rows[row_index];

    if (currow->dirty == 0) {
        currow->len = 0;
        currow->buf = realloc(currow->buf, currow->len);
    }

    char *newbuf = realloc(currow->buf, currow->len + len);
    if (newbuf == NULL) {
        perror("realloc");
        return -1;
    }

    int prev_srows_len = getSrowLen(row_index);

    memcpy(newbuf + prev_srows_len + info.cursor_x + len - 1, newbuf + prev_srows_len + info.cursor_x - 1, currow->len - prev_srows_len - info.cursor_x + 1);
    memcpy(newbuf + prev_srows_len + info.cursor_x - 1, buf, len);

    currow->dirty = 1;
    currow->len += len;
    currow->buf = newbuf;

    // This part removes welcome message if any word is pressed and entered in screen.
    erow *welcome_msg_row = &info.rows[info.screen_row / 2 - 2];
    if (welcome_msg_row->dirty == 0 && welcome_msg_row->len > 1) {
        welcome_msg_row->len = 1;
        welcome_msg_row->buf = realloc(welcome_msg_row->buf, welcome_msg_row->len);
    }

    updateSrows();

    return len;
}

void removeErow(int row_index)
{
    erow prevrow = info.rows[row_index - 1];
    erow currow = info.rows[row_index];

    if (currow.dirty) {
        prevrow.buf = realloc(prevrow.buf, prevrow.len + currow.len);
        memcpy(prevrow.buf + prevrow.len, currow.buf, currow.len);
        prevrow.len += currow.len;
    }

    int tmprows_size = (info.numrows - row_index - 1) * sizeof(erow);
    erow *tmprows = (erow *) malloc(tmprows_size);
    memcpy(tmprows, info.rows + row_index + 1, tmprows_size);

    erow *newrows = realloc(info.rows, (info.numrows - 1) * sizeof(erow));
    memcpy(newrows + row_index, tmprows, tmprows_size);
    newrows[row_index - 1] = prevrow;

    info.rows = newrows;
    info.numrows -= 1;

    info.cursor_y -= 1;
    if (currow.dirty)
        info.cursor_x = prevrow.len - currow.len + 1;
    else
        info.cursor_x = prevrow.len + 1;
}

// mode = {DELETE_KEY or BACKSPACE}
int deleteCharacter(int mode, int len)
{
    srow cursrow = info.srows[info.cursor_y + info.cursor_y_offset- 1];
    int row_index = cursrow.row_index;
    erow *currow = &info.rows[row_index];

    if (info.cursor_x == 1 && info.cursor_y == 1) {
        return -1;
    }

    if (mode == BACKSPACE && info.cursor_x == 1 && info.cursor_y > 1) {
        removeErow(row_index);

        updateSrows();
        return -2;
    }

    if (currow->len - len == 0) {
        currow->dirty = 0;
        currow->len = 1;
        currow->buf = realloc(currow->buf, currow->len);
        memcpy(currow->buf, "~", 1);

        return len;
    }

    int offset;
    if (mode == DELETE_KEY)
        offset = 1;
    else if (mode == BACKSPACE)
        offset = 2;

    int prev_srows_len = getSrowLen(row_index);

    char *tmpbuf = (char *) malloc(currow->len - prev_srows_len - info.cursor_x - len + offset);
    if (tmpbuf == NULL) {
        perror("realloc");
        return -1;
    }
    char *newbuf = realloc(currow->buf, currow->len - len);
    if (newbuf == NULL) {
        perror("realloc");
        return -1;
    }

    memcpy(tmpbuf, currow->buf + prev_srows_len + info.cursor_x + len - offset, currow->len - prev_srows_len - info.cursor_x - len + offset);
    memcpy(newbuf + info.cursor_x - offset, tmpbuf, currow->len - prev_srows_len - info.cursor_x - len + offset);

    currow->len -= len;
    currow->buf = newbuf;

    free(tmpbuf);

    updateSrows();
    return len;
}

int enter()
{
    srow cursrow = info.srows[info.cursor_y + info.cursor_y_offset- 1];
    int row_index = cursrow.row_index;
    erow currow = info.rows[row_index];

    if (currow.dirty == 0) {
        currow.len = 0;
        currow.buf = realloc(currow.buf, currow.len);
    }

    int prev_srows_len = getSrowLen(row_index);

    erow newrow, edittedrow;
    newrow.dirty = 1;
    newrow.len = currow.len - prev_srows_len - info.cursor_x + 1;
    newrow.buf = (char *) malloc(newrow.len);

    edittedrow.dirty = 1;
    edittedrow.len = prev_srows_len + info.cursor_x - 1;
    edittedrow.buf = (char *) malloc(edittedrow.len);

    // This part removes welcome message if any word is pressed and entered in screen.
    erow *welcome_msg_row = &info.rows[info.screen_row / 2 - 2];
    if (welcome_msg_row->dirty == 0 && welcome_msg_row->len > 1) {
        welcome_msg_row->len = 1;
        welcome_msg_row->buf = realloc(welcome_msg_row->buf, welcome_msg_row->len);
    }

    memcpy(newrow.buf, currow.buf + prev_srows_len + info.cursor_x - 1, newrow.len);
    memcpy(edittedrow.buf, currow.buf, edittedrow.len);

    erow *newrows = realloc(info.rows, (info.numrows + 1) * sizeof(erow));
    if (newrows == NULL) {
        perror("realloc");
        return -1;
    }

    memcpy(newrows + row_index + 2, newrows + row_index + 1, (info.numrows - row_index - 1) * sizeof(erow));

    newrows[row_index] = edittedrow;
    newrows[row_index + 1] = newrow;

    info.rows = newrows;
    info.numrows += 1;

    updateSrows();

    return 0;
}

int addRowToErows(int dirty, char *buf, int len)
{
    erow newrow;
    newrow.dirty = dirty;
    newrow.len = len;
    newrow.buf = buf;

    erow *newrows = realloc(info.rows, (info.numrows + 1) * sizeof(erow));
    if (newrows == NULL) {
        perror("realloc");
        return -1;
    }

    newrows[info.numrows] = newrow;

    info.rows = newrows;

    info.numrows += 1;

    return 1;
}

/*
 * This function is update erows using cbuf's contents.
 *
 * It is called when cbuf is changed, initial state of program or open existing file for example.
 *
 * In for loop, if statement checks whether the character in cbuf is '\n' or reaching cbuf's end.
 *
 * In final step, it makes new string buffer and add that buffer to erow by calling addRowToErows().
 */
void updateRows(int dirty, struct cbuf cb)
{
    int len_count = cb.len;     // Track the remained cbuf length.
    int buflen;
    int is_entered;             // Check Enter key.

    while (len_count > 0) {
        is_entered = 0;
        for (int i = 0; i < info.screen_col; i++) {
            buflen = i + 1;
            if (cb.buf[cb.len - len_count + i] == '\n') {
                is_entered = 1;
                break;
            }
            else if (i == len_count - 1) {
                break;
            }
        }
        char *newbuf = (char *) malloc(is_entered ? buflen - 2 : buflen);
        if (newbuf == NULL) {
            return;
        }

        memcpy(newbuf, cb.buf + (cb.len - len_count), is_entered ? buflen - 2 : buflen);
        addRowToErows(dirty, newbuf, is_entered ? buflen - 2 : buflen);

        len_count -= buflen;
    }

    updateSrows();
}

void initScreenNoArgs(struct cbuf *cb)
{
    char *welcome_msg = "Welcome to mzim! This program is the vim for mz gen!";
    int welcome_len = 52;
    int i = 0;
    for (; i < info.screen_row - 1; i++) {
        addToCbuf(cb, "~", 1);

        if (i == info.screen_row / 2 - 2) {
            for (int j = 0; j < (info.screen_col / 2) - (welcome_len / 2); j++) {
                addToCbuf(cb, " ", 1);
            }
            addToCbuf(cb, welcome_msg, welcome_len);
            addToCbuf(cb, "\r\n", 2);
        } else if (i < info.screen_row) {
            addToCbuf(cb, "\r\n", 2);
        }
    }
    addToCbuf(cb, "~", 1);

    updateRows(0, *cb);
}

/*
 * Open Existing file by using argument when execute program. (./mzim <filename>)
 *
 * This function will return 0 if it works well, otherwise return 1.
 */
int openFile(struct cbuf *cb)
{
    FILE *fp = fopen(info.filename, "r");
    if (fp == NULL) {
        perror("fopen");
        return 1;
    }

    char *c = (char *)malloc(sizeof(char));
    int ret;
    while ((ret = fread(c, 1, 1, fp)) == 1) {
        if (strcmp(c, "\n") == 0) {
            addToCbuf(cb, "\r", 1);
        }
        addToCbuf(cb, c, 1);
    }

    updateRows(1, *cb);

    free(c);
    return 0;
}

void clearScreen()
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void moveCursor(int key)
{
    if (info.rows[info.srows[info.cursor_y + info.cursor_y_offset - 1].row_index].dirty == 0)
        return;

    switch(key) {
        case ARROW_UP:
            if (info.cursor_y > 1) {
                info.cursor_y -= 1;
            }
            else if (info.cursor_y_offset > 0) {
                info.cursor_y_offset -= 1;
            }
            if (info.srows[info.cursor_y + info.cursor_y_offset - 1].len < info.cursor_x)
                    info.cursor_x = info.srows[info.cursor_y + info.cursor_y_offset - 1].len + 1;

            break;
        case ARROW_DOWN:
            if (info.cursor_y + info.cursor_y_offset < info.numsrows) {
                if (info.cursor_y >= info.screen_row)
                    info.cursor_y_offset += 1;
                else
                    info.cursor_y += 1;
            }

            if (info.srows[info.cursor_y + info.cursor_y_offset - 1].len < info.cursor_x)
                    info.cursor_x = info.srows[info.cursor_y + info.cursor_y_offset - 1].len + 1;

            break;
        case ARROW_RIGHT:
            if (info.cursor_x <= info.srows[info.cursor_y + info.cursor_y_offset - 1].len)
                info.cursor_x += 1;
            break;
        case ARROW_LEFT:
            if (info.cursor_x > 1)
                info.cursor_x -= 1;
            break;
    }
}

void drawCursor()
{
    char buf[10];
    snprintf(buf, 10, "\x1b[%d;%dH", info.cursor_y, info.cursor_x);
    write(STDOUT_FILENO, buf, 10);
}

void drawContentRow()
{
    clearScreen();

    int i;
    for (i = 0; i < info.screen_row - 1; i++) {
        write(STDOUT_FILENO, info.srows[i + info.cursor_y_offset].buf, info.srows[i + info.cursor_y_offset].len);
        write(STDOUT_FILENO, "\n\r", 2);
    }
    write(STDOUT_FILENO, info.srows[i + info.cursor_y_offset].buf, info.srows[i + info.cursor_y_offset].len);

    drawCursor();
}

/*
 * This function will return integer which means entered key.
 *
 * There will be not only normal input key but also escape key(page up/down, home/end, delete, arrow keys).
 */
int getKey()
{
    int ret;
    char c;
    while((ret = read(STDIN_FILENO, &c, 1)) != 1) {
        if (ret == -1)
            die("read");
    }

    if (c == '\x1b') {
        char seq[3];

        /* Two if statement below check that the key is valid. */
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '3': return DELETE_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                    }
                }
            } else {
                switch(seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }

        return '\x1b';
    }
    else if (c == 13) {
        return ENTER_KEY;
    }
    else if (c == 127) {
        return BACKSPACE;
    }
    else if (c == 9) {
        return TAB_KEY;
    }
    return c;
}

void quit_action()
{
    for (int i = 0; i < info.numrows; i++) {
        erow *currow = &info.rows[i];
        free(currow->buf);
    }
    free(info.rows);
    free(info.srows);

    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void manageKeyInput()
{
    int c = getKey();
    char* buf = (char *) malloc(sizeof(char));

    switch(c) {
        case CTRL_KEY('q'):
            quit_action();
            exit(0);
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        {
            int iter = info.screen_row;
            while(--iter) {
                moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        }
            break;

        case HOME_KEY:
            info.cursor_x = 1;
            break;
        case END_KEY:
            info.cursor_x = info.srows[info.cursor_y + info.cursor_y_offset - 1].len + 1;
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_RIGHT:
        case ARROW_LEFT:
            moveCursor(c);
            break;

        case DELETE_KEY:
            deleteCharacter(DELETE_KEY, 1);
            break;

        case ENTER_KEY:
            enter();
            moveCursor(ARROW_DOWN);
            info.cursor_x = 1;
            break;

        case BACKSPACE:
            if (deleteCharacter(BACKSPACE, 1) != -2)
                moveCursor(ARROW_LEFT);
            break;

        case TAB_KEY:
            for (int i = 0; i < 4; i++) {
                addCharacter(" ", 1);
                moveCursor(ARROW_RIGHT);
            }
            break;

        default:
            // implement printing input
            buf[0] = (char) c;
            addCharacter(buf, 1);

            if (info.cursor_x == info.screen_col) {
                info.cursor_y += 1;
                info.cursor_x = info.srows[info.cursor_y + info.cursor_y_offset - 1].len + 1;
            }
            else {
                moveCursor(ARROW_RIGHT);
            }
    }

    free(buf);
}

void getWindowSize()
{
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    info.screen_row = w.ws_row;
    info.screen_col = w.ws_col;
}

void initializeEditorInfo()
{
    info.filename = NULL;

    info.screen_row = 0;
    info.screen_col = 0;

    info.cursor_x = 1;
    info.cursor_y = 1;
    info.cursor_y_offset = 0;

    info.numrows = 0;
    info.rows = NULL;

    info.numsrows = 0;
    info.srows = NULL;
}

int main(int argc, char **argv)
{
    struct cbuf cb = CBUF_INIT;

    initializeEditorInfo();
    getWindowSize();

    if (argc == 1) {
        initScreenNoArgs(&cb);
    } else if (argc > 1) {
        info.filename = argv[1];
        openFile(&cb);
    }

    enableRawMode();

    while (1) {
        getWindowSize(&info.screen_row, &info.screen_col);
        drawContentRow();
        manageKeyInput();
    }
    return 0;
}
