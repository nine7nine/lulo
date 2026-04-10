#define _GNU_SOURCE

#include "lulo_app.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long mono_ms_now_local(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

const char *input_backend_name(InputBackend backend)
{
    switch (backend) {
    case INPUT_BACKEND_NOTCURSES:
        return "notcurses";
    case INPUT_BACKEND_RAW:
        return "raw";
    case INPUT_BACKEND_AUTO:
    default:
        return "auto";
    }
}

int parse_input_backend(const char *text, InputBackend *backend)
{
    if (!text || !backend) return -1;
    if (strcmp(text, "auto") == 0) *backend = INPUT_BACKEND_AUTO;
    else if (strcmp(text, "nc") == 0 || strcmp(text, "notcurses") == 0) *backend = INPUT_BACKEND_NOTCURSES;
    else if (strcmp(text, "raw") == 0) *backend = INPUT_BACKEND_RAW;
    else return -1;
    return 0;
}

InputBackend auto_input_backend(void)
{
    const char *term = getenv("TERM");

    if (term && (!strcmp(term, "vte-256color") || !strcmp(term, "gnome"))) {
        return INPUT_BACKEND_NOTCURSES;
    }
    return INPUT_BACKEND_RAW;
}

void debug_log_open(DebugLog *log)
{
    const char *path;

    memset(log, 0, sizeof(*log));
    path = getenv("LULO_DEBUG_INPUT");
    if (!path || !*path) return;
    log->fp = fopen(path, "w");
    if (!log->fp) return;
    log->enabled = 1;
    fprintf(log->fp, "opened\n");
    fflush(log->fp);
}

void debug_log_close(DebugLog *log)
{
    if (!log || !log->enabled) return;
    fprintf(log->fp, "closed\n");
    fclose(log->fp);
    log->fp = NULL;
    log->enabled = 0;
}

void debug_log_stage(DebugLog *log, const char *stage)
{
    if (!log || !log->enabled) return;
    fprintf(log->fp, "stage %s\n", stage);
    fflush(log->fp);
}

void debug_log_errno(DebugLog *log, const char *tag)
{
    if (!log || !log->enabled) return;
    fprintf(log->fp, "%s errno=%d %s\n", tag, errno, strerror(errno));
    fflush(log->fp);
}

void debug_log_poll(DebugLog *log, const char *tag, int fd, int revents)
{
    if (!log || !log->enabled) return;
    fprintf(log->fp, "%s fd=%d revents=0x%x\n", tag, fd, revents);
    fflush(log->fp);
}

void debug_log_message(DebugLog *log, const char *tag, const char *value)
{
    if (!log || !log->enabled) return;
    fprintf(log->fp, "%s %s\n", tag, value ? value : "");
    fflush(log->fp);
}

void debug_log_nc_event(DebugLog *log, const char *tag, uint32_t id, const ncinput *ni)
{
    if (!log || !log->enabled) return;
    fprintf(log->fp,
            "%s id=%u evtype=%d y=%d x=%d mods=%u utf8='%s' eff0=%d eff1=%d\n",
            tag, id, ni ? (int)ni->evtype : -1, ni ? ni->y : -1, ni ? ni->x : -1,
            ni ? ni->modifiers : 0, ni ? ni->utf8 : "",
            ni ? ni->eff_text[0] : 0, ni ? ni->eff_text[1] : 0);
    fflush(log->fp);
}

void debug_log_bytes(DebugLog *log, const char *tag, const unsigned char *buf, size_t len)
{
    if (!log || !log->enabled) return;
    fprintf(log->fp, "%s len=%zu", tag, len);
    for (size_t i = 0; i < len; i++) fprintf(log->fp, " %02x", buf[i]);
    fputc('\n', log->fp);
    fflush(log->fp);
}

void debug_log_action(DebugLog *log, const char *tag, const DecodedInput *in)
{
    if (!log || !log->enabled || !in) return;
    fprintf(log->fp,
            "%s action=%d mouse_press=%d mouse_release=%d mouse_wheel=%d repeat=%d x=%d y=%d button=%d\n",
            tag, in->action, in->mouse_press, in->mouse_release, in->mouse_wheel,
            in->key_repeat, in->mouse_x, in->mouse_y, in->mouse_button);
    fflush(log->fp);
}

static void terminal_write_escape(const char *seq)
{
    if (!seq || !*seq) return;
    (void)!write(STDOUT_FILENO, seq, strlen(seq));
}

void terminal_mouse_enable(void)
{
    terminal_write_escape("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
}

void terminal_mouse_disable(void)
{
    terminal_write_escape("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
}

int raw_input_enable(RawInput *in)
{
    struct termios raw;

    memset(in, 0, sizeof(*in));
    if (tcgetattr(STDIN_FILENO, &in->old_tc) < 0) return -1;
    raw = in->old_tc;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return -1;
    in->active = 1;
    return 0;
}

void raw_input_disable(RawInput *in)
{
    if (!in || !in->active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &in->old_tc);
    in->active = 0;
    in->len = 0;
}

ssize_t raw_input_fill(RawInput *in)
{
    ssize_t total = 0;

    if (!in || in->len >= sizeof(in->buf)) return 0;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, in->buf + in->len, sizeof(in->buf) - in->len);
        if (n > 0) {
            if (in->len == 0) in->first_ms = mono_ms_now_local();
            in->len += (size_t)n;
            total += n;
            if (in->len >= sizeof(in->buf)) break;
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return total > 0 ? total : -1;
    }
    return total;
}

static void raw_input_consume(RawInput *in, size_t count)
{
    if (count >= in->len) {
        in->len = 0;
        in->first_ms = 0;
        return;
    }
    memmove(in->buf, in->buf + count, in->len - count);
    in->len -= count;
}

static InputAction decode_key_byte(unsigned char ch)
{
    switch (ch) {
    case '\t':
        return INPUT_TAB_NEXT;
    case 'q':
    case 'Q':
    case 0x1b:
        return INPUT_QUIT;
    case '+':
    case '=':
        return INPUT_SAMPLE_FASTER;
    case '-':
    case '_':
        return INPUT_SAMPLE_SLOWER;
    case 'p':
    case 'P':
        return INPUT_TOGGLE_PROC_CPU;
    case 'r':
        return INPUT_CYCLE_PROC_REFRESH;
    case 'R':
        return INPUT_RELOAD_PAGE;
    case 'n':
    case 'N':
        return INPUT_NEW_ITEM;
    case 'd':
    case 'D':
        return INPUT_DELETE_SELECTED;
    case 'm':
    case 'M':
        return INPUT_RENAME_SELECTED;
    case 'f':
    case 'F':
        return INPUT_TOGGLE_FOCUS;
    case 'k':
    case 'K':
        return INPUT_SCROLL_UP;
    case 'j':
    case 'J':
        return INPUT_SCROLL_DOWN;
    case ' ':
    case '\r':
    case '\n':
        return INPUT_TOGGLE_BRANCH;
    case 'x':
        return INPUT_SIGNAL_TERM;
    case 'X':
        return INPUT_SIGNAL_KILL;
    case 'c':
    case 'C':
        return INPUT_COLLAPSE_ALL;
    case 'e':
    case 'E':
        return INPUT_EXPAND_ALL;
    case 's':
        return INPUT_SAVE_SNAPSHOT;
    case 'S':
        return INPUT_SAVE_PRESET;
    case 'i':
    case 'I':
        return INPUT_EDIT_SELECTED;
    case 'a':
    case 'A':
        return INPUT_APPLY_SELECTED;
    case '?':
        return INPUT_TOGGLE_HELP;
    default:
        return INPUT_NONE;
    }
}

static int parse_decimal_field(const unsigned char *buf, size_t len, size_t *pos, int *value)
{
    int v = 0;
    size_t i = *pos;

    if (i >= len || !isdigit(buf[i])) return 0;
    while (i < len && isdigit(buf[i])) {
        v = v * 10 + (buf[i] - '0');
        i++;
    }
    *pos = i;
    *value = v;
    return 1;
}

static int raw_input_decode_csi(RawInput *in, DecodedInput *out)
{
    const unsigned char *buf = in->buf;
    size_t len = in->len;

    if (len < 3 || buf[0] != 0x1b || buf[1] != '[') return 0;
    if (buf[2] == 'A') {
        out->action = INPUT_SCROLL_UP;
        raw_input_consume(in, 3);
        return 1;
    }
    if (buf[2] == 'B') {
        out->action = INPUT_SCROLL_DOWN;
        raw_input_consume(in, 3);
        return 1;
    }
    if (buf[2] == 'C') {
        out->action = INPUT_NONE;
        raw_input_consume(in, 3);
        return 1;
    }
    if (buf[2] == 'D') {
        out->action = INPUT_NONE;
        raw_input_consume(in, 3);
        return 1;
    }
    if (buf[2] == 'H') {
        out->action = INPUT_HOME;
        raw_input_consume(in, 3);
        return 1;
    }
    if (buf[2] == 'F') {
        out->action = INPUT_END;
        raw_input_consume(in, 3);
        return 1;
    }
    if (buf[2] == 'Z') {
        out->action = INPUT_VIEW_NEXT;
        raw_input_consume(in, 3);
        return 1;
    }
    if (buf[2] == '<') {
        size_t pos = 3;
        int b = 0;
        int x = 0;
        int y = 0;
        int release = 0;
        int press = 0;
        int wheel = 0;
        int button = 0;

        if (!parse_decimal_field(buf, len, &pos, &b)) return 0;
        if (pos >= len || buf[pos] != ';') return 0;
        pos++;
        if (!parse_decimal_field(buf, len, &pos, &x)) return 0;
        if (pos >= len || buf[pos] != ';') return 0;
        pos++;
        if (!parse_decimal_field(buf, len, &pos, &y)) return 0;
        if (pos >= len) return 0;
        if (buf[pos] == 'm') release = 1;
        else if (buf[pos] == 'M') press = 1;
        else return 0;
        pos++;

        if ((b & 64) != 0) {
            wheel = 1;
            if ((b & 3) == 2) {
                button = 6;
                out->action = INPUT_NONE;
            } else if ((b & 3) == 3) {
                button = 7;
                out->action = INPUT_NONE;
            } else {
                button = (b & 1) ? 5 : 4;
                out->action = (button == 4) ? INPUT_SCROLL_UP : INPUT_SCROLL_DOWN;
            }
            out->mouse_wheel = 1;
        } else {
            button = (b & 3) + 1;
            out->action = INPUT_NONE;
        }
        out->mouse_x = x;
        out->mouse_y = y;
        out->mouse_button = button;
        out->mouse_press = press && !wheel;
        out->mouse_release = release && !wheel;
        raw_input_consume(in, pos);
        return 1;
    }
    if (isdigit(buf[2])) {
        size_t pos = 2;
        int code = 0;

        if (!parse_decimal_field(buf, len, &pos, &code)) return 0;
        if (pos >= len) return 0;
        if (buf[pos] == '~') {
            switch (code) {
            case 1:
            case 7:
                out->action = INPUT_HOME;
                break;
            case 4:
            case 8:
                out->action = INPUT_END;
                break;
            case 5:
                out->action = INPUT_PAGE_UP;
                break;
            case 6:
                out->action = INPUT_PAGE_DOWN;
                break;
            default:
                return 0;
            }
            raw_input_consume(in, pos + 1);
            return 1;
        }
        if (buf[pos] == ';') {
            int mod = 0;

            pos++;
            if (!parse_decimal_field(buf, len, &pos, &mod)) return 0;
            if (pos >= len || buf[pos] == ';') return 0;
            switch (buf[pos]) {
            case 'A': out->action = INPUT_SCROLL_UP; break;
            case 'B': out->action = INPUT_SCROLL_DOWN; break;
            case 'C': out->action = INPUT_NONE; break;
            case 'D': out->action = INPUT_NONE; break;
            case 'H': out->action = INPUT_HOME; break;
            case 'F': out->action = INPUT_END; break;
            default: return 0;
            }
            out->key_repeat = mod >= 2;
            raw_input_consume(in, pos + 1);
            return 1;
        }
    }
    return 0;
}

int raw_input_decode_one(RawInput *in, DecodedInput *out)
{
    memset(out, 0, sizeof(*out));
    if (!in || in->len == 0) return 0;
    if (in->buf[0] == 0x1b) {
        if (raw_input_decode_csi(in, out)) return 1;
        if (in->len == 1) {
            if (in->first_ms > 0 && mono_ms_now_local() - in->first_ms >= 80) {
                out->action = INPUT_NONE;
                out->cancel = 1;
                raw_input_consume(in, 1);
                return 1;
            }
            return 0;
        }
        if (in->buf[1] == 'O') {
            if (in->len < 3) return 0;
            switch (in->buf[2]) {
            case 'A':
                out->action = INPUT_SCROLL_UP;
                break;
            case 'B':
                out->action = INPUT_SCROLL_DOWN;
                break;
            case 'C':
                out->action = INPUT_NONE;
                break;
            case 'D':
                out->action = INPUT_NONE;
                break;
            case 'H':
                out->action = INPUT_HOME;
                break;
            case 'F':
                out->action = INPUT_END;
                break;
            default:
                break;
            }
            raw_input_consume(in, 3);
            return 1;
        }
        if (in->buf[1] != '[') {
            out->action = INPUT_NONE;
            out->cancel = 1;
            raw_input_consume(in, 1);
            return 1;
        }
        raw_input_consume(in, 1);
        return 0;
    }
    if (in->buf[0] == 0x7f || in->buf[0] == 0x08) {
        out->backspace = 1;
        raw_input_consume(in, 1);
        return 1;
    }
    if (in->buf[0] == '\r' || in->buf[0] == '\n') {
        out->action = INPUT_TOGGLE_BRANCH;
        out->submit = 1;
        raw_input_consume(in, 1);
        return 1;
    }
    if (isprint(in->buf[0])) {
        out->text[0] = (char)in->buf[0];
        out->text[1] = '\0';
        out->text_len = 1;
    }
    out->action = decode_key_byte(in->buf[0]);
    raw_input_consume(in, 1);
    return out->action != INPUT_NONE || out->text_len > 0;
}

InputAction decode_notcurses_input(uint32_t id)
{
    switch (id) {
    case 'q':
    case 'Q':
        return INPUT_QUIT;
    case '+':
    case '=':
        return INPUT_SAMPLE_FASTER;
    case '-':
    case '_':
        return INPUT_SAMPLE_SLOWER;
    case 'p':
        return INPUT_TOGGLE_PROC_CPU;
    case 'r':
        return INPUT_CYCLE_PROC_REFRESH;
    case 'R':
        return INPUT_RELOAD_PAGE;
    case 'n':
    case 'N':
        return INPUT_NEW_ITEM;
    case 'd':
    case 'D':
        return INPUT_DELETE_SELECTED;
    case 'm':
    case 'M':
        return INPUT_RENAME_SELECTED;
    case 'f':
        return INPUT_TOGGLE_FOCUS;
    case 'k':
    case NCKEY_UP:
        return INPUT_SCROLL_UP;
    case 'j':
    case NCKEY_DOWN:
        return INPUT_SCROLL_DOWN;
    case NCKEY_LEFT:
        return INPUT_NONE;
    case NCKEY_RIGHT:
        return INPUT_NONE;
    case NCKEY_TAB:
        return INPUT_TAB_NEXT;
    case NCKEY_PGUP:
        return INPUT_PAGE_UP;
    case NCKEY_PGDOWN:
        return INPUT_PAGE_DOWN;
    case NCKEY_HOME:
        return INPUT_HOME;
    case NCKEY_END:
        return INPUT_END;
    case ' ':
    case NCKEY_ENTER:
        return INPUT_TOGGLE_BRANCH;
    case 'x':
        return INPUT_SIGNAL_TERM;
    case 'X':
        return INPUT_SIGNAL_KILL;
    case 'c':
    case 'C':
        return INPUT_COLLAPSE_ALL;
    case 'e':
    case 'E':
        return INPUT_EXPAND_ALL;
    case 's':
        return INPUT_SAVE_SNAPSHOT;
    case 'S':
        return INPUT_SAVE_PRESET;
    case 'i':
    case 'I':
        return INPUT_EDIT_SELECTED;
    case 'a':
    case 'A':
        return INPUT_APPLY_SELECTED;
    case '?':
        return INPUT_TOGGLE_HELP;
    case NCKEY_RESIZE:
        return INPUT_RESIZE;
    case NCKEY_SCROLL_UP:
        return INPUT_SCROLL_UP;
    case NCKEY_SCROLL_DOWN:
        return INPUT_SCROLL_DOWN;
    default:
        return INPUT_NONE;
    }
}
