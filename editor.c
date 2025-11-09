#define _POSIX_C_SOURCE 200809L        // enable POSIX stuff like getline on some libc

#include <ctype.h>                     // isprint, isspace, etc.
#include <errno.h>                     // errno, strerror
#include <fcntl.h>                     // open, O_* flags
#include <stdarg.h>                    // va_list for formatted status messages
#include <stdbool.h>                   // bool type
#include <stdio.h>                    // printf-like, FILE, getline
#include <stdlib.h>                   // malloc, realloc, free, exit
#include <string.h>                   // memcpy, memmove, strlen, strcmp
#include <termios.h>                  // terminal raw mode
#include <time.h>                     // time for status message timeout
#include <unistd.h>                   // read, write, close, fsync
#include <sys/ioctl.h>                // ioctl for window size

/* ----------------------- Config / Macros ----------------------- */

#define CTRL_KEY(k) ((k) & 0x1f)       // turn uppercase letter into control-code (e.g. 'Q' -> Ctrl-Q)
#define EDITOR_VERSION "0.3-linux-fixed" // version shown in status bar
#define STATUS_MSG_SEC 5               // how long status message stays visible

/* ----------------------- Safe write helper ----------------------- */
/* We use a wrapper so we never partially write to stdout and leave garbage. */

static void xwrite(int fd, const void *buf, size_t n) {
    const char *p = (const char*)buf;  // pointer to current position in buffer
    size_t off = 0;                    // number of bytes already written
    while (off < n) {                  // loop until all bytes are written
        ssize_t r = write(fd, p + off, n - off); // try to write remaining bytes
        if (r < 0) {                   // if write failed
            if (errno == EINTR)        // if it was interrupted by signal
                continue;              // try again
            perror("write");           // otherwise print error
            _exit(1);                  // and exit immediately (can't recover)
        }
        off += (size_t)r;              // advance by number of bytes written
    }
}

/* ----------------------- Data structures ----------------------- */
/* We model the file as a dynamic array of lines; each line is a NUL-terminated string. */

typedef struct {
    char **lines;                      // array of pointers to lines
    int  *len;                         // array of lengths for each line (excluding NUL)
    int   count;                       // how many lines are currently used
    int   cap;                         // how many lines we can store without realloc
} Buffer;

typedef struct {
    int cx, cy;                        // cursor position in text coordinates (col, row)
    int rowoff;                        // vertical scroll: first row currently shown
    int coloff;                        // horizontal scroll: first column currently shown (not fully used here)
    int screenrows;                    // how many rows of text we can show
    int screencols;                    // how many columns we can show
    int pref_cx;                       // preferred column when moving up/down (to keep column when lines differ)
} View;

/* ----------------------- Globals ----------------------- */

static struct termios orig_termios;    // original terminal settings to restore on exit
static Buffer buf = {0};               // the text buffer
static View   view = {0};              // the view (scroll + cursor)
static bool   dirty = false;           // whether buffer has unsaved changes
static char   filename[256] = "untitled.txt"; // current filename

static char   statusmsg[256] = "";     // current status message
static time_t statusmsg_time = 0;      // when we set the status message

/* search state */
static char last_query[256] = "";      // last search query (for Ctrl-N)
static int  last_match_row = -1;       // row of last match
static int  last_match_col = -1;       // column of last match

/* temporary highlight for search result */
static int hl_row = -1, hl_col = -1, hl_len = 0; // highlight location and length

/* ask for Ctrl-Q twice if dirty */
static int quit_times_needed = 1;      // if dirty, require 2 Ctrl-Q

/* ----------------------- Terminal handling ----------------------- */

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); // restore old terminal settings
}

static void die(const char *msg) {
    xwrite(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); // clear screen and go home
    disable_raw_mode();               // restore terminal
    perror(msg);                      // print reason
    exit(1);                          // terminate
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) // get current terminal attrs
        die("tcgetattr");                              // abort on failure
    struct termios raw = orig_termios;                 // start from current settings

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // turn off misc input flags
    raw.c_oflag &= ~(OPOST);                                 // turn off all output post-processing
    raw.c_cflag |= (CS8);                                    // 8-bit chars
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);         // turn off echo, canonical mode, signals

    raw.c_cc[VMIN]  = 0;                                     // read returns as soon as there is input
    raw.c_cc[VTIME] = 1;                                     // ...or after 0.1s

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)      // set new settings
        die("tcsetattr");                                    // abort on failure
}

/*
 * Read a key from stdin and return an integer representing it.
 * We try to decode escape sequences like arrows and PageUp/Down
 * in a tolerant way (WSL / slow terminal can split bytes).
 */
static int editor_read_key(void) {
    char c;                                      // char read
    while (1) {                                  // loop until we get something
        ssize_t n = read(STDIN_FILENO, &c, 1);   // read 1 byte
        if (n == 1) break;                       // got it
        if (n == -1 && errno != EAGAIN)          // real error (not EAGAIN)
            die("read");                         // abort
        /* if n == 0 or EAGAIN we just loop, terminal is non-blocking-ish */
    }

    if (c != '\x1b')                             // if it's not ESC, it's a regular key
        return c;                                // return it as-is

    /* we saw ESC, so let's try to read the rest of the sequence */
    char seq[16];                                // buffer for escape sequence
    int n = 0;                                   // length read

    while (n < (int)sizeof(seq)) {               // read until buffer is full
        ssize_t r = read(STDIN_FILENO, &seq[n], 1); // try to read next byte
        if (r != 1)                              // if no more bytes right now
            break;                               // stop collecting
        n++;                                     // we got one more
        if ((seq[n-1] >= 'A' && seq[n-1] <= 'Z') // many CSI sequences end with letter
            || seq[n-1] == '~')                  // or with tilde
            break;                               // stop collecting
    }

    if (n == 0)                                  // lone ESC: no sequence
        return '\x1b';                           // return ESC

    if (seq[0] == '[') {                         // CSI sequence starting with '['
        if (n == 2) {                            // typical arrow form: ESC [ A
            switch (seq[1]) {                    // check final char
                case 'A': return 1001;           // Up
                case 'B': return 1002;           // Down
                case 'C': return 1003;           // Right
                case 'D': return 1004;           // Left
                case 'H': return 'H';            // Home
                case 'F': return 'E';            // End
            }
        } else if (n >= 3 && seq[1] >= '0' && seq[1] <= '9') { // ESC [ 1 ~ kind
            int num = 0;                         // numeric part
            for (int i = 1; i < n-1; i++) {      // parse digits
                if (seq[i] >= '0' && seq[i] <= '9')
                    num = num*10 + (seq[i]-'0'); // accumulate
            }
            switch (num) {                       // map numeric code
                case 1:
                case 7: return 'H';              // Home
                case 4:
                case 8: return 'E';              // End
                case 3: return 127;              // Delete
                case 5: return 1005;             // PageUp
                case 6: return 1006;             // PageDown
            }
        }
    } else if (seq[0] == 'O') {                  // ESC O ... alternative arrow set
        if (n == 2) {
            switch (seq[1]) {
                case 'A': return 1001;           // Up
                case 'B': return 1002;           // Down
                case 'C': return 1003;           // Right
                case 'D': return 1004;           // Left
                case 'H': return 'H';            // Home
                case 'F': return 'E';            // End
            }
        }
    }

    return '\x1b';                               // fallback: return ESC
}

/* Get terminal window size using ioctl */
static int get_window_size(int *rows, int *cols) {
    struct winsize ws;                           // structure to hold size
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0 || ws.ws_row == 0)
        return -1;                               // failed
    *cols = ws.ws_col;                           // set columns
    *rows = ws.ws_row;                           // set rows
    return 0;                                    // success
}

/* ----------------------- Status & messages ----------------------- */

static void editor_set_status(const char *fmt, ...) {
    va_list ap;                                  // variadic args
    va_start(ap, fmt);                           // start collecting
    vsnprintf(statusmsg, sizeof(statusmsg), fmt, ap); // write formatted message
    va_end(ap);                                  // stop
    statusmsg_time = time(NULL);                 // remember when we set it
}

/* ----------------------- Buffer management ----------------------- */

/* Initialize buffer with 1 empty line so editor always has at least one line */
static void buffer_init(Buffer *b) {
    b->cap = 64;                                 // initial capacity
    b->count = 1;                                // start with 1 line
    b->lines = (char**)calloc(b->cap, sizeof(char*)); // allocate line array
    b->len   = (int*)calloc(b->cap, sizeof(int));     // allocate length array
    b->lines[0] = strdup("");                    // first line is empty string
    b->len[0] = 0;                               // length is 0
}

/* Free buffer content */
static void buffer_free(Buffer *b) {
    if (!b || !b->lines) return;                 // nothing to free
    for (int i = 0; i < b->count; i++)           // free each line
        free(b->lines[i]);
    free(b->lines);                              // free array of line pointers
    free(b->len);                                // free array of lengths
    memset(b, 0, sizeof(*b));                    // clear structure
}

/* Ensure we have space for at least 'need' lines */
static void buffer_ensure_capacity(Buffer *b, int need) {
    if (need <= b->cap) return;                  // capacity is enough
    while (b->cap < need)                        // grow exponentially
        b->cap *= 2;
    b->lines = (char**)realloc(b->lines, b->cap * sizeof(char*)); // grow line array
    b->len   = (int*)realloc(b->len,   b->cap * sizeof(int));     // grow len array
}

/* Insert a line at position 'at' with content 's' of length 'n' */
static void buffer_insert_line(Buffer *b, int at, const char *s, int n) {
    if (at < 0 || at > b->count) return;         // out of range
    buffer_ensure_capacity(b, b->count + 1);     // make space for new line
    memmove(&b->lines[at+1], &b->lines[at], (b->count - at) * sizeof(char*)); // shift lines down
    memmove(&b->len[at+1],   &b->len[at],   (b->count - at) * sizeof(int));   // shift lengths down
    b->lines[at] = (char*)malloc(n + 1);         // allocate new line
    memcpy(b->lines[at], s, n);                  // copy content
    b->lines[at][n] = '\0';                      // terminate string
    b->len[at] = n;                              // set length
    b->count++;                                  // one more line
}

/* Helper: append an empty line at the bottom (used for Arrow Down create-line) */
static void buffer_append_empty(Buffer *b) {
    buffer_insert_line(b, b->count, "", 0);      // insert empty string at end
}

/* Insert a single character into a line at (row, col) */
static void buffer_insert_char(Buffer *b, int row, int col, char c) {
    int L = b->len[row];                         // current line length
    if (col < 0) col = 0;                        // clamp column
    if (col > L) col = L;                        // clamp to end
    b->lines[row] = (char*)realloc(b->lines[row], L + 2); // +1 char +1 NUL
    memmove(&b->lines[row][col+1], &b->lines[row][col], L - col + 1); // shift right incl. NUL
    b->lines[row][col] = c;                      // insert char
    b->len[row] = L + 1;                         // update length
    dirty = true;                                // mark buffer dirty
}

/* Delete char at (row, col-1) -> typical backspace behavior */
static void buffer_delete_char(Buffer *b, int row, int col) {
    int L = b->len[row];                         // line length
    if (col <= 0 || col > L) return;             // nothing to delete
    memmove(&b->lines[row][col-1], &b->lines[row][col], L - col + 1); // shift left
    b->len[row] = L - 1;                         // update length
    dirty = true;                                // mark dirty
}

/* Split a line into two at column 'col' (used for Enter) */
static void buffer_split_line(Buffer *b, int row, int col) {
    int L = b->len[row];                         // line length
    if (col < 0) col = 0;                        // clamp
    if (col > L) col = L;                        // clamp
    const char *right = &b->lines[row][col];     // pointer to right part
    buffer_insert_line(b, row + 1, right, L - col); // insert right part as new line
    b->lines[row][col] = '\0';                   // truncate original line
    b->len[row] = col;                           // update length
    dirty = true;                                // mark dirty
}

/* Join current line with previous one (used for backspace at col 0) */
static void buffer_join_with_prev(Buffer *b, int row) {
    if (row <= 0 || row >= b->count) return;     // can't join
    int Lp = b->len[row - 1];                    // length of previous line
    int Lc = b->len[row];                        // length of current line
    b->lines[row - 1] = (char*)realloc(b->lines[row - 1], Lp + Lc + 1); // extend prev
    memcpy(&b->lines[row - 1][Lp], b->lines[row], Lc + 1); // copy current + NUL
    b->len[row - 1] = Lp + Lc;                   // update length

    free(b->lines[row]);                         // free current line
    memmove(&b->lines[row], &b->lines[row + 1], (b->count - row - 1) * sizeof(char*)); // shift up
    memmove(&b->len[row],   &b->len[row + 1],   (b->count - row - 1) * sizeof(int));   // shift lengths
    b->count--;                                  // one line less
    dirty = true;                                // mark dirty
}

/* ----------------------- File I/O ----------------------- */
/* Load file into buffer as lines */
static void editor_open(const char *path) {
    FILE *f = fopen(path, "rb");                 // open file for binary read
    if (!f) {                                    // if file doesn't exist
        snprintf(filename, sizeof(filename), "%s", path); // just remember name
        return;                                  // start with empty buffer
    }

    buffer_free(&buf);                           // clear existing buffer
    buffer_init(&buf);                           // start fresh

    char *line = NULL;                           // getline buffer
    size_t cap = 0;                              // getline capacity
    ssize_t n;                                   // read length
    bool first = true;                           // special-case first line

    while ((n = getline(&line, &cap, f)) != -1) { // read each line
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) // strip newlines
            n--;
        if (first) {                             // first line replaces the initial empty line
            free(buf.lines[0]);                  // free empty line
            buf.lines[0] = (char*)malloc(n + 1); // allocate exactly
            memcpy(buf.lines[0], line, n);       // copy data
            buf.lines[0][n] = '\0';              // terminate
            buf.len[0] = (int)n;                 // set length
            first = false;                       // no longer first
        } else {
            buffer_insert_line(&buf, buf.count, line, (int)n); // append line
        }
    }
    free(line);                                  // free getline buffer
    fclose(f);                                   // close file
    snprintf(filename, sizeof(filename), "%s", path); // remember file name
    dirty = false;                               // clean state
}

/* Atomic save: write to tmp + fsync + rename */
static bool editor_save_atomic(void) {
    char tmpname[300];                           // buffer for tmp filename
    snprintf(tmpname, sizeof(tmpname), "%s.tmp", filename); // tmp = filename.tmp

    int fd = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC, 0644); // open tmp
    if (fd == -1) return false;                  // failed to open tmp

    bool ok = true;                              // track success
    for (int i = 0; i < buf.count; i++) {        // write each line
        if (write(fd, buf.lines[i], buf.len[i]) == -1) { ok = false; break; } // write line
        if (write(fd, "\n", 1) == -1) { ok = false; break; }                  // newline
    }
    if (ok && fsync(fd) == -1) ok = false;       // flush to disk
    if (close(fd) == -1) ok = false;             // close file
    if (!ok) { unlink(tmpname); return false; }  // cleanup on failure

    if (rename(tmpname, filename) == -1) {       // atomically replace real file
        unlink(tmpname);                         // remove tmp
        return false;                            // report failure
    }

    dirty = false;                               // buffer is clean now
    return true;                                 // success
}

/* ----------------------- View / Rendering ----------------------- */

/* Update view dimensions from terminal */
static bool editor_update_dimensions(void) {
    int rows = 24;                               // default rows if ioctl fails
    int cols = 80;                               // default cols
    if (get_window_size(&rows, &cols) == -1) {   // try to get real size
        if (view.screenrows > 0)                 // if we already have previous size
            rows = view.screenrows + 2;          // approximate
        if (view.screencols > 0)                 // same for cols
            cols = view.screencols;
    }
    int textrows = rows - 2;                     // leave 2 lines: status + message
    if (textrows < 1) textrows = 1;              // clamp
    bool changed = (textrows != view.screenrows) || (cols != view.screencols); // did it change?
    view.screenrows = textrows;                  // store rows
    view.screencols = cols;                      // store cols
    return changed;                              // return whether it changed
}

/* Return how far we are in file as percent (for status bar) */
static int percent_through(void) {
    if (buf.count <= 1) return 100;              // single-line file: show 100%
    int p = (view.cy + 1) * 100 / buf.count;     // simple proportion
    if (p < 1) p = 1;                            // clamp low
    if (p > 100) p = 100;                        // clamp high
    return p;                                    // return percentage
}

/* Adjust scroll so cursor is visible */
static void editor_scroll(void) {
    if (view.cy < view.rowoff)                   // if cursor above top
        view.rowoff = view.cy;                   // scroll up
    if (view.cy >= view.rowoff + view.screenrows) // if cursor below bottom
        view.rowoff = view.cy - view.screenrows + 1; // scroll down

    if (view.cx < view.coloff)                   // if cursor left of left edge
        view.coloff = view.cx;                   // scroll left
    if (view.cx >= view.coloff + view.screencols) // if cursor right of right edge
        view.coloff = view.cx - view.screencols + 1; // scroll right
}

/* Draw a single line considering highlight and horizontal offset */
static void draw_line_with_highlight(int filerow) {
    int left = view.coloff;                      // starting column
    int maxw = view.screencols;                  // max width we can draw
    int len = buf.len[filerow] - left;           // visible part length
    if (len < 0) len = 0;                        // nothing to show
    if (len > maxw) len = maxw;                  // clamp to screen

    if (len == 0) return;                        // empty line: nothing to draw

    if (filerow == hl_row && hl_len > 0 && hl_col >= 0) { // if this line has highlight
        int hstart = hl_col - left;              // highlight start in screen coords
        int hend   = hl_col + hl_len - left;     // highlight end in screen coords

        if (hend <= 0 || hstart >= maxw) {       // highlight is outside screen
            xwrite(STDOUT_FILENO, &buf.lines[filerow][left], len); // draw normal
            return;                              // done
        }
        if (hstart < 0) hstart = 0;              // clamp start
        if (hend > len) hend = len;              // clamp end

        if (hstart > 0)                          // draw text before highlight
            xwrite(STDOUT_FILENO, &buf.lines[filerow][left], hstart);
        xwrite(STDOUT_FILENO, "\x1b[7m", 4);      // turn on inverse video
        xwrite(STDOUT_FILENO, &buf.lines[filerow][left + hstart], hend - hstart); // draw highlighted part
        xwrite(STDOUT_FILENO, "\x1b[m", 3);       // reset attributes
        if (hend < len)                          // draw text after highlight
            xwrite(STDOUT_FILENO, &buf.lines[filerow][left + hend], len - hend);
    } else {
        xwrite(STDOUT_FILENO, &buf.lines[filerow][left], len); // normal draw
    }
}

/* Draw whole screen (text area + status + message) */
static void editor_draw_screen(void) {
    xwrite(STDOUT_FILENO, "\x1b[?25l\x1b[H", 8);  // hide cursor and move to top-left
    editor_scroll();                             // make sure cursor is in viewport

    for (int y = 0; y < view.screenrows; y++) {  // draw every visible text row
        int filerow = view.rowoff + y;           // actual file row index
        xwrite(STDOUT_FILENO, "\x1b[2K\r", 5);   // clear current line
        if (filerow < buf.count)                 // if there is a file line
            draw_line_with_highlight(filerow);   // draw that line
        else
            xwrite(STDOUT_FILENO, "", 0);        // no '~', just leave it empty
        xwrite(STDOUT_FILENO, "\r\n", 2);        // go to next terminal line
    }

    xwrite(STDOUT_FILENO, "\x1b[7m", 4);         // start inverted for status bar
    char left[160], right[80];                   // buffers for status parts
    snprintf(left,  sizeof(left),  " %.40s %s", filename, dirty ? "(modified)" : ""); // left side: filename + dirty
    snprintf(right, sizeof(right), " %d:%d %3d%% v%s ",
             view.cy + 1, view.cx + 1, percent_through(), EDITOR_VERSION); // right side: pos + percent + version

    int len = (int)strlen(left);                 // length of left part
    if (len > view.screencols) len = view.screencols; // clamp
    xwrite(STDOUT_FILENO, left, len);            // write left part

    int right_len = (int)strlen(right);          // length of right part
    while (len < view.screencols - right_len) {  // pad with spaces between left and right
        xwrite(STDOUT_FILENO, " ", 1);
        len++;
    }
    if (right_len > view.screencols) right_len = view.screencols; // clamp right part
    xwrite(STDOUT_FILENO, right, right_len);    // write right part
    xwrite(STDOUT_FILENO, "\x1b[m", 3);         // end inverted
    xwrite(STDOUT_FILENO, "\r\n", 2);           // move to message line

    xwrite(STDOUT_FILENO, "\x1b[2K\r", 5);      // clear message line
    if (statusmsg[0] && (time(NULL) - statusmsg_time) < STATUS_MSG_SEC) { // if message is fresh
        int msglen = (int)strlen(statusmsg);     // length of message
        if (msglen > view.screencols) msglen = view.screencols; // clamp
        xwrite(STDOUT_FILENO, statusmsg, msglen); // write message
    }

    int scr_y = view.cy - view.rowoff;           // cursor y on screen
    int scr_x = view.cx - view.coloff;           // cursor x on screen
    if (scr_y < 0) scr_y = 0;                    // clamp
    if (scr_y >= view.screenrows) scr_y = view.screenrows - 1;
    if (scr_x < 0) scr_x = 0;
    if (scr_x >= view.screencols) scr_x = view.screencols - 1;

    dprintf(STDOUT_FILENO, "\x1b[%d;%dH", scr_y + 1, scr_x + 1); // move cursor to text area
    xwrite(STDOUT_FILENO, "\x1b[?25h", 6);        // show cursor again
}

/* ----------------------- Movement & Editing ----------------------- */

/* Move by page (PageUp/PageDown) */
static void editor_move_cursor_vert(int key) {
    int page = view.screenrows - 2;              // how many lines to move
    if (page < 1) page = 1;                      // at least 1

    if (key == 1005) {                           // PageUp
        view.cy -= page;                         // move up
        if (view.cy < 0) view.cy = 0;            // clamp to top
    } else {                                     // PageDown
        view.cy += page;                         // move down
        if (view.cy >= buf.count) view.cy = buf.count - 1; // clamp to last line
    }

    int L = buf.len[view.cy];                    // length of new line
    view.cx = (view.pref_cx <= L) ? view.pref_cx : L; // restore preferred col
}

/* Move cursor for arrows/Home/End; create new line on Down at EOF */
static void editor_move_cursor(int key) {
    switch (key) {
        case 1004: {                             // Left
            if (view.cx > 0) {                   // if not at start of line
                view.cx--;                       // move left
                view.pref_cx = view.cx;          // update preferred col
            } else if (view.cy > 0) {            // if at start but not first line
                view.cy--;                       // go to previous line
                view.cx = buf.len[view.cy];      // to its end
                view.pref_cx = view.cx;          // update preferred col
            }
        } break;
        case 1003: {                             // Right
            if (view.cy < buf.count) {           // if there is a line
                int L = buf.len[view.cy];        // current line length
                if (view.cx < L) {               // if not at end
                    view.cx++;                   // move right
                    view.pref_cx = view.cx;      // update
                } else if (view.cy + 1 < buf.count) { // if at end but there is next line
                    view.cy++;                   // go to next line
                    view.cx = 0;                 // at its start
                    view.pref_cx = 0;            // update
                }
            }
        } break;
        case 1001: {                             // Up
            if (view.cy > 0)                     // if not top line
                view.cy--;                       // move up
            int L = buf.len[view.cy];            // new line length
            view.cx = (view.pref_cx <= L) ? view.pref_cx : L; // clamp to line end
        } break;
        case 1002: {                             // Down
            if (view.cy + 1 < buf.count) {       // if there is a line below
                view.cy++;                       // go down
            } else {                             // we are on last line
                buffer_append_empty(&buf);       // create a new empty line
                view.cy++;                       // move to it
            }
            int L = buf.len[view.cy];            // length of new line
            view.cx = (view.pref_cx <= L) ? view.pref_cx : L; // restore preferred col
        } break;
        case 'H': {                              // Home
            view.cx = 0;                         // go to start of line
            view.pref_cx = 0;                    // update preferred col
        } break;
        case 'E': {                              // End
            view.cx = buf.len[view.cy];          // go to end of line
            view.pref_cx = view.cx;              // update preferred col
        } break;
    }
}

/* Insert printable char at cursor */
static void editor_insert_char(int c) {
    buffer_insert_char(&buf, view.cy, view.cx, (char)c); // insert char into buffer
    view.cx++;                              // move cursor right
    view.pref_cx = view.cx;                // update preferred col
    hl_row = hl_col = hl_len = -1;         // clear search highlight
}

/* Insert newline at cursor */
static void editor_insert_newline(void) {
    buffer_split_line(&buf, view.cy, view.cx); // split line at cursor
    view.cy++;                                 // move cursor to new line
    view.cx = 0;                               // at start of new line
    view.pref_cx = 0;                          // update preferred col
    hl_row = hl_col = hl_len = -1;             // clear highlight
}

/* Delete char before cursor or join lines */
static void editor_backspace(void) {
    if (view.cx > 0) {                         // if not at start of line
        buffer_delete_char(&buf, view.cy, view.cx); // delete char
        view.cx--;                              // move cursor left
        view.pref_cx = view.cx;                 // update
    } else if (view.cy > 0) {                   // at start but not first line
        int prev_len = buf.len[view.cy - 1];    // length of previous line
        buffer_join_with_prev(&buf, view.cy);   // merge current into previous
        view.cy--;                              // move cursor to previous line
        view.cx = prev_len;                     // at the join point
        view.pref_cx = view.cx;                 // update
    }
    hl_row = hl_col = hl_len = -1;             // clear highlight
}

/* ----------------------- Prompt & Search ----------------------- */

/* Simple prompt at bottom that returns a string (like "/" in vim) */
static bool editor_prompt(const char *prompt, char *out, size_t outlen) {
    size_t n = 0;                              // current length of user input
    out[0] = '\0';                             // start empty
    while (1) {                                // loop until user confirms or cancels
        editor_set_status("%s%s", prompt, out); // show prompt + current input
        editor_draw_screen();                  // redraw
        int c = editor_read_key();             // read key
        if (c == '\x1b') {                     // ESC -> cancel
            editor_set_status("Canceled");     // say canceled
            return false;                      // signal cancel
        }
        if (c == '\r' || c == '\n') {          // Enter
            if (n > 0) {                       // only accept if user typed something
                editor_set_status("");         // clear status
                return true;                   // success
            }
        } else if (c == 127) {                 // Backspace inside prompt
            if (n > 0) {                       // if we have chars
                out[--n] = '\0';               // drop last char
            }
        } else if (isprint(c)) {               // printable
            if (n + 1 < outlen) {              // if we have room
                out[n++] = (char)c;            // append char
                out[n] = '\0';                 // keep NUL
            }
        }
    }
}

/* Find next occurrence of last_query; from_current = search from current cursor */
static bool editor_find_next(bool from_current) {
    if (last_query[0] == '\0') return false;   // nothing to search

    int r = from_current ? view.cy : last_match_row;             // start row
    int c = from_current ? view.cx : (last_match_col + 1);       // start col

    for (int round = 0; round < 2; round++) { // 2 rounds to wrap around
        for (; r < buf.count; r++, c = 0) {    // scan rows
            const char *hay = buf.lines[r];    // line text
            if (buf.len[r] == 0) continue;     // skip empty lines
            const char *p = strstr(hay + (c < buf.len[r] ? c : buf.len[r]), last_query); // find substring
            if (p) {                           // found
                last_match_row = r;            // remember match row
                last_match_col = (int)(p - hay); // remember match col
                hl_row = r; hl_col = last_match_col; hl_len = (int)strlen(last_query); // set highlight
                view.cy = r;                   // move cursor to match
                view.pref_cx = view.cx = last_match_col; // set col
                return true;                   // success
            }
        }
        r = 0; c = 0;                          // wrap to top
    }
    return false;                              // not found
}

/* Ask user for query and search first occurrence */
static void editor_find(void) {
    char query[256] = "";                      // buffer for query
    if (!editor_prompt("/", query, sizeof(query))) { // if user cancels
        hl_row = hl_col = hl_len = -1;         // clear highlight
        return;                                // done
    }
    snprintf(last_query, sizeof(last_query), "%s", query); // store last query
    last_match_row = view.cy;                  // start from current
    last_match_col = view.cx - 1;              // will be incremented by search
    if (!editor_find_next(true)) {             // try to find
        editor_set_status("Not found: %s", last_query); // message
        hl_row = hl_col = hl_len = -1;         // clear highlight
    } else {
        editor_set_status("Found: %s  (Ctrl-N for next)", last_query); // message
    }
}

/* ----------------------- Main loop ----------------------- */

int main(int argc, char **argv) {
    atexit(disable_raw_mode);                  // make sure raw mode is off at exit
    enable_raw_mode();                         // enter raw mode

    buffer_init(&buf);                         // initialize buffer with 1 empty line
    if (argc >= 2) {                           // if file provided
        snprintf(filename, sizeof(filename), "%s", argv[1]); // remember name
        editor_open(argv[1]);                  // load file
    }

    view.cx = view.cy = 0;                     // cursor at start
    view.pref_cx = 0;                          // preferred col 0
    view.rowoff = view.coloff = 0;             // no scroll yet
    editor_update_dimensions();                // get terminal size
    editor_set_status("HELP: type | Enter | Backspace | Ctrl-S save | Ctrl-F find | Ctrl-N next | Ctrl-Q quit"); // initial help
    editor_draw_screen();                      // first draw

    while (1) {                                // main loop
        int c = editor_read_key();             // read key

        bool request_redraw = true;            // by default we redraw
        bool exit_editor = false;              // track exit

        if (c == CTRL_KEY('q')) {              // Ctrl-Q
            if (dirty && quit_times_needed > 0) { // if unsaved changes and still need confirmation
                editor_set_status("Unsaved changes — press Ctrl-Q again to quit"); // warn
                quit_times_needed--;           // decrease counter
            } else {
                xwrite(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); // clear screen on exit
                exit_editor = true;            // exit loop
                request_redraw = false;        // no need to redraw
            }
        } else if (c == CTRL_KEY('s')) {       // Ctrl-S save
            if (editor_save_atomic())          // try to save
                editor_set_status("Saved: %s", filename); // success
            else
                editor_set_status("Save failed: %s", strerror(errno)); // error
            quit_times_needed = 1;             // reset quit counter
        } else if (c == CTRL_KEY('f')) {       // Ctrl-F search
            editor_find();                     // run search
            quit_times_needed = 1;             // reset quit counter
        } else if (c == CTRL_KEY('n')) {       // Ctrl-N next match
            if (!editor_find_next(false))      // find next after last match
                editor_set_status("No more matches for: %s", last_query); // message
            quit_times_needed = 1;             // reset
        } else if (c == 1005 || c == 1006) {   // PageUp / PageDown
            editor_move_cursor_vert(c);        // move by page
            quit_times_needed = 1;             // reset
            hl_row = hl_col = hl_len = -1;     // clear highlight
        } else if (c == 1001 || c == 1002 || c == 1003 || c == 1004 || c == 'H' || c == 'E') {
            editor_move_cursor(c);             // move cursor
            quit_times_needed = 1;             // reset
            hl_row = hl_col = hl_len = -1;     // clear highlight
        } else if (c == '\r' || c == '\n') {   // Enter
            editor_insert_newline();           // split line
            quit_times_needed = 1;             // reset
        } else if (c == 127) {                 // Backspace / Delete
            editor_backspace();                // delete char
            quit_times_needed = 1;             // reset
        } else if (isprint(c)) {               // printable char
            editor_insert_char(c);             // insert
            quit_times_needed = 1;             // reset
        } else {
            request_redraw = false;            // unknown key: no need to redraw
        }

        if (exit_editor)                       // if we decided to exit
            break;                             // break main loop

        if (editor_update_dimensions())        // if window size changed
            request_redraw = true;             // force redraw
        if (request_redraw)                    // if we decided to redraw
            editor_draw_screen();              // draw full screen
    }

    buffer_free(&buf);                         // free buffer
    return 0;                                  // done
}
