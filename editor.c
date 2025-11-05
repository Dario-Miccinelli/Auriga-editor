/*
-------------------------------------------------
This is a minimal text editor in plain C that runs in a terminal without external libraries.
Think of it as a handmade notebook:
- terminal "raw mode" = silence the terminal's default behavior so we can read keys byte-by-byte
- ANSI escape sequences = move the camera, clear, draw, and place the cursor
- a dynamic array of lines = our paper sheets
- a cursor (cx, cy) and a viewport (rowoff) = the pen and the window into the page
- basic editing: printable insert, Enter to split lines, Backspace to delete/merge
- Ctrl+S to save, Ctrl+Q to quit

Limitations today (to be fixed in the next days):
- only simple UTF-8 (we treat bytes; no IME, no tabs as wide characters)
- horizontal scrolling is minimal (we clamp to screen width)
- no search/undo yet
*/

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>   // POSIX terminal control (raw mode)
#include <time.h>
#include <unistd.h>    // read/write

/* ----------------------- Config & Macros ----------------------- */

#define CTRL_KEY(k) ((k) & 0x1f)
#define EDITOR_VERSION "0.1"
#define STATUS_MSG_SEC 5

/* We keep lines as NUL-terminated C strings; len[] stores logical lengths. */
typedef struct {
    char **lines;   // array of pointers to lines
    int  *len;      // current length of each line (without trailing '\0')
    int   count;    // number of lines
    int   cap;      // capacity (lines array)
} Buffer;

typedef struct {
    int cx, cy;        // cursor (column, row) in text coordinates
    int rowoff;        // vertical scroll offset (first row shown)
    int coloff;        // horizontal scroll offset (future use)
    int screenrows;    // usable rows for text (excluding status)
    int screencols;    // usable columns
} View;

/* Global editor state (kept simple for MVP) */
static struct termios orig_termios;
static Buffer buf = {0};
static View   view = {0};
static bool   dirty = false;
static char   filename[256] = "untitled.txt";
static char   statusmsg[256] = "";
static time_t statusmsg_time = 0;

/* ----------------------- Low-level Terminal ----------------------- */

/* Handle fatal errors: restore terminal and exit. */
static void die(const char *msg) {
    // try to restore terminal before exiting
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); // clear screen, move home
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    perror(msg);
    exit(1);
}

/* Enable raw mode: no echo, no canonical line buffering, etc. */
static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    struct termios raw = orig_termios;

    // Input modes: no CR->NL, no XON/XOFF, no special byte translations
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // Output: no post-processing (we handle newlines)
    raw.c_oflag &= ~(OPOST);
    // Control: set character size to 8 bits
    raw.c_cflag |= (CS8);
    // Local: turn off echo, canonical mode, extended functions, and signals
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    // Read returns as soon as 1 byte is available, with short timeout
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1; // tenths of a second

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/* Disable raw mode (called at exit). */
static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

/* Read a single key (or escape sequence) from stdin. */
static int editor_read_key(void) {
    char c;
    while (1) {
        ssize_t nread = read(STDIN_FILENO, &c, 1);
        if (nread == 1) break;
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // Handle escape sequences for arrows/home/end
    if (c == '\x1b') {
        char seq[3];

        // read the next two bytes if available
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // extended codes: ESC [ 1 ~ etc. (we try to consume one more)
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return 'H'; // Home
                        case '4': return 'E'; // End (fallback)
                        case '3': return 127; // Delete (treat as backspace fallback)
                        case '5': return 1005; // PageUp (custom code)
                        case '6': return 1006; // PageDown
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return 1001; // Up
                    case 'B': return 1002; // Down
                    case 'C': return 1003; // Right
                    case 'D': return 1004; // Left
                    case 'H': return 'H';  // Home
                    case 'F': return 'E';  // End
                }
            }
        }

        return '\x1b';
    }

    return c;
}

/* Get terminal size (rows, cols) using cursor position trick. */
static int get_window_size(int *rows, int *cols) {
    // Use ioctl(TIOCGWINSZ) in real life; here we use a simple ANSI fallback
    // Cursor to bottom-right, then query position.
    char bufq[32];
    unsigned int i = 0;

    // Ask for cursor position: ESC [ 6 n
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) == -1) return -1;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) == -1) return -1;

    // Read response: ESC [ rows ; cols R
    while (i < sizeof(bufq) - 1) {
        if (read(STDIN_FILENO, &bufq[i], 1) != 1) break;
        if (bufq[i] == 'R') break;
        i++;
    }
    bufq[i] = '\0';

    if (bufq[0] != '\x1b' || bufq[1] != '[') return -1;
    if (sscanf(&bufq[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

/* ----------------------- Buffer (text storage) ----------------------- */

static void buffer_init(Buffer *b) {
    b->cap = 64;
    b->count = 1;
    b->lines = (char**)calloc(b->cap, sizeof(char*));
    b->len   = (int*)calloc(b->cap, sizeof(int));
    b->lines[0] = strdup("");
    b->len[0] = 0;
}

/* Ensure capacity for inserting lines. */
static void buffer_ensure_capacity(Buffer *b, int need) {
    if (need <= b->cap) return;
    while (b->cap < need) b->cap *= 2;
    b->lines = (char**)realloc(b->lines, b->cap * sizeof(char*));
    b->len   = (int*)realloc(b->len,   b->cap * sizeof(int));
}

/* Insert a new line at index 'at' with a copy of [s..s+n). */
static void buffer_insert_line(Buffer *b, int at, const char *s, int n) {
    if (at < 0 || at > b->count) return;
    buffer_ensure_capacity(b, b->count + 1);
    memmove(&b->lines[at+1], &b->lines[at], (b->count - at) * sizeof(char*));
    memmove(&b->len[at+1],   &b->len[at],   (b->count - at) * sizeof(int));
    b->lines[at] = (char*)malloc(n + 1);
    memcpy(b->lines[at], s, n);
    b->lines[at][n] = '\0';
    b->len[at] = n;
    b->count++;
}

/* Insert one printable char at (row,col). */
static void buffer_insert_char(Buffer *b, int row, int col, char c) {
    if (row < 0 || row >= b->count) return;
    int L = b->len[row];
    if (col < 0) col = 0;
    if (col > L) col = L;
    b->lines[row] = (char*)realloc(b->lines[row], L + 2);
    memmove(&b->lines[row][col+1], &b->lines[row][col], L - col + 1);
    b->lines[row][col] = c;
    b->len[row] = L + 1;
    dirty = true;
}

/* Delete char before (row,col). */
static void buffer_delete_char(Buffer *b, int row, int col) {
    if (row < 0 || row >= b->count) return;
    int L = b->len[row];
    if (col <= 0 || col > L) return;
    memmove(&b->lines[row][col-1], &b->lines[row][col], L - col + 1);
    b->len[row] = L - 1;
    dirty = true;
}

/* Split current line at col into two lines. */
static void buffer_split_line(Buffer *b, int row, int col) {
    if (row < 0 || row >= b->count) return;
    int L = b->len[row];
    if (col < 0) col = 0;
    if (col > L) col = L;
    const char *right = &b->lines[row][col];
    buffer_insert_line(b, row + 1, right, L - col);
    b->lines[row][col] = '\0';
    b->len[row] = col;
    dirty = true;
}

/* Join line 'row' to previous one (removes 'row'). */
static void buffer_join_with_prev(Buffer *b, int row) {
    if (row <= 0 || row >= b->count) return;
    int Lp = b->len[row - 1];
    int Lc = b->len[row];
    b->lines[row - 1] = (char*)realloc(b->lines[row - 1], Lp + Lc + 1);
    memcpy(&b->lines[row - 1][Lp], b->lines[row], Lc + 1);
    b->len[row - 1] = Lp + Lc;

    free(b->lines[row]);
    memmove(&b->lines[row], &b->lines[row + 1], (b->count - row - 1) * sizeof(char*));
    memmove(&b->len[row],   &b->len[row + 1],   (b->count - row - 1) * sizeof(int));
    b->count--;
    dirty = true;
}

/* ----------------------- File I/O ----------------------- */

static void editor_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        // New file: keep single empty line; set filename
        snprintf(filename, sizeof(filename), "%s", path);
        return;
    }

    // Reset buffer
    for (int i = 0; i < buf.count; i++) free(buf.lines[i]);
    free(buf.lines); free(buf.len);
    buffer_init(&buf);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    bool first = true;

    while ((n = getline(&line, &cap, f)) != -1) {
        // strip trailing \n or \r\n
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
        if (first) {
            free(buf.lines[0]);
            buf.lines[0] = (char*)malloc(n + 1);
            memcpy(buf.lines[0], line, n);
            buf.lines[0][n] = '\0';
            buf.len[0] = (int)n;
            first = false;
        } else {
            buffer_insert_line(&buf, buf.count, line, (int)n);
        }
    }

    free(line);
    fclose(f);
    snprintf(filename, sizeof(filename), "%s", path);
    dirty = false;
}

static bool editor_save(void) {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return false;

    for (int i = 0; i < buf.count; i++) {
        if (write(fd, buf.lines[i], buf.len[i]) == -1) { close(fd); return false; }
        if (write(fd, "\n", 1) == -1) { close(fd); return false; }
    }
    close(fd);
    dirty = false;
    return true;
}

/* ----------------------- Rendering (ANSI) ----------------------- */

static void editor_set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(statusmsg, sizeof(statusmsg), fmt, ap);
    va_end(ap);
    statusmsg_time = time(NULL);
}

/* Draw text rows and status bar using ANSI. */
static void editor_draw_screen(void) {
    // Hide cursor & move home
    write(STDOUT_FILENO, "\x1b[?25l\x1b[H", 8);

    // Draw each row of the viewport
    for (int y = 0; y < view.screenrows - 1; y++) {
        int filerow = view.rowoff + y;
        // Clear line
        write(STDOUT_FILENO, "\x1b[2K\r", 5);
        if (filerow >= buf.count) {
            // Tilde lines like vi; show filename at first empty line
            if (buf.count == 1 && buf.len[0] == 0 && y == 0) {
                dprintf(STDOUT_FILENO, "~  %s", filename);
            } else {
                write(STDOUT_FILENO, "~", 1);
            }
        } else {
            int len = buf.len[filerow] - view.coloff;
            if (len < 0) len = 0;
            if (len > view.screencols) len = view.screencols;
            write(STDOUT_FILENO, &buf.lines[filerow][view.coloff], len);
        }
        write(STDOUT_FILENO, "\r\n", 2);
    }

    // Status bar (inverse colors)
    write(STDOUT_FILENO, "\x1b[7m", 4);
    char status[256], rstatus[64];
    snprintf(status, sizeof(status), " %.20s %s", filename, dirty ? "(modified)" : "");
    snprintf(rstatus, sizeof(rstatus), " %d/%d  v%s ",
             view.cy + 1, buf.count, EDITOR_VERSION);
    int len = (int)strlen(status);
    if (len > view.screencols) len = view.screencols;
    write(STDOUT_FILENO, status, len);
    while (len < view.screencols - (int)strlen(rstatus)) {
        write(STDOUT_FILENO, " ", 1); len++;
    }
    write(STDOUT_FILENO, rstatus, strlen(rstatus));
    write(STDOUT_FILENO, "\x1b[m", 3);
    write(STDOUT_FILENO, "\r\n", 2);

    // Status message (fades after N seconds)
    if (statusmsg[0] && (time(NULL) - statusmsg_time) < STATUS_MSG_SEC) {
        int msglen = (int)strlen(statusmsg);
        if (msglen > view.screencols) msglen = view.screencols;
        write(STDOUT_FILENO, "\x1b[2K\r", 5);
        write(STDOUT_FILENO, statusmsg, msglen);
        write(STDOUT_FILENO, "\r\n", 2);
    } else {
        write(STDOUT_FILENO, "\x1b[2K\r\n", 5);
    }

    // Place cursor (screen coords)
    int scr_y = view.cy - view.rowoff;
    int scr_x = view.cx - view.coloff;
    if (scr_y < 0) scr_y = 0;
    if (scr_y >= view.screenrows - 1) scr_y = view.screenrows - 2;
    if (scr_x < 0) scr_x = 0;
    if (scr_x >= view.screencols) scr_x = view.screencols - 1;

    // Move cursor (1-based)
    dprintf(STDOUT_FILENO, "\x1b[%d;%dH", scr_y + 1, scr_x + 1);
    // Show cursor
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}

/* Keep cursor within bounds; adjust vertical scroll. */
static void editor_scroll(void) {
    if (view.cy < view.rowoff) view.rowoff = view.cy;
    if (view.cy >= view.rowoff + view.screenrows - 1)
        view.rowoff = view.cy - view.screenrows + 2;
    if (view.coloff < 0) view.coloff = 0;
}

/* ----------------------- Editing ops & movement ----------------------- */

static void move_cursor(int key) {
    switch (key) {
        case 1004: // Left
            if (view.cx > 0) view.cx--;
            else if (view.cy > 0) { view.cy--; view.cx = buf.len[view.cy]; }
            break;
        case 1003: // Right
            if (view.cy < buf.count) {
                int L = buf.len[view.cy];
                if (view.cx < L) view.cx++;
                else if (view.cy + 1 < buf.count) { view.cy++; view.cx = 0; }
            }
            break;
        case 1001: // Up
            if (view.cy > 0) view.cy--;
            if (view.cx > buf.len[view.cy]) view.cx = buf.len[view.cy];
            break;
        case 1002: // Down
            if (view.cy + 1 < buf.count) view.cy++;
            if (view.cx > buf.len[view.cy]) view.cx = buf.len[view.cy];
            break;
        case 'H': // Home
            view.cx = 0; break;
        case 'E': // End
            view.cx = buf.len[view.cy]; break;
    }
}

static void insert_char(int c) {
    buffer_insert_char(&buf, view.cy, view.cx, (char)c);
    view.cx++;
}

static void insert_newline(void) {
    buffer_split_line(&buf, view.cy, view.cx);
    view.cy++; view.cx = 0;
}

static void backspace_op(void) {
    if (view.cx > 0) {
        buffer_delete_char(&buf, view.cy, view.cx);
        view.cx--;
    } else if (view.cy > 0) {
        int prev_len = buf.len[view.cy - 1];
        buffer_join_with_prev(&buf, view.cy);
        view.cy--; view.cx = prev_len;
    }
}

/* ----------------------- Init & Main loop ----------------------- */

static void editor_init_dimensions(void) {
    int rows = 24, cols = 80;
    get_window_size(&rows, &cols);
    // Reserve one row for status; one extra line for messages
    view.screenrows = rows - 2;
    view.screencols = cols;
}

int main(int argc, char **argv) {
    atexit(disable_raw_mode);
    enable_raw_mode();

    buffer_init(&buf);
    if (argc >= 2) {
        snprintf(filename, sizeof(filename), "%s", argv[1]);
        editor_open(argv[1]);
    }

    editor_init_dimensions();
    editor_set_status("HELP: Ctrl-S = save | Ctrl-Q = quit");

    while (1) {
        editor_scroll();
        editor_draw_screen();

        int c = editor_read_key();
        if (c == 0) continue;

        if (c == CTRL_KEY('q')) {
            // clear screen before exit
            write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
            break;
        } else if (c == CTRL_KEY('s')) {
            if (editor_save())
                editor_set_status("Saved to %s", filename);
            else
                editor_set_status("Save failed: %s", strerror(errno));
        } else if (c == 1001 || c == 1002 || c == 1003 || c == 1004 || c == 'H' || c == 'E') {
            move_cursor(c);
        } else if (c == '\r' || c == '\n') {
            insert_newline();
        } else if (c == 127) {
            backspace_op();
        } else if (isprint(c)) {
            insert_char(c);
        }

        // Re-read window size occasionally (could also handle SIGWINCH)
        editor_init_dimensions();
    }

    return 0;
}
