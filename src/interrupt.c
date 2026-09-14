#include "interrupt.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#define ESC 0x1b

static volatile int g_interrupted = 0;
static int g_watching = 0;
static int g_saved_flags = -1;
static int g_saved_termios_ok = 0;
static struct termios g_saved_termios;

void interrupt_clear(void)
{
    g_interrupted = 0;
}

int interrupt_pending(void)
{
    return g_interrupted != 0;
}

/*
 * Put the terminal into a non-canonical, non-blocking mode so a lone ESC key
 * press is delivered immediately instead of waiting for a newline. The saved
 * state is restored by interrupt_watch_end().
 */
void interrupt_watch_begin(void)
{
    if (g_watching)
        return;
    g_watching = 1;
    interrupt_clear();

    if (!isatty(STDIN_FILENO))
        return;

    if (tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
        g_saved_termios_ok = 1;
        struct termios raw = g_saved_termios;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    g_saved_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (g_saved_flags != -1)
        fcntl(STDIN_FILENO, F_SETFL, g_saved_flags | O_NONBLOCK);
}

int interrupt_poll(void)
{
    if (g_interrupted)
        return 1;
    if (!g_watching || !isatty(STDIN_FILENO))
        return 0;

    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == ESC) {
            g_interrupted = 1;
            return 1;
        }
    }
    return 0;
}

void interrupt_watch_end(void)
{
    if (!g_watching)
        return;
    g_watching = 0;

    if (!isatty(STDIN_FILENO))
        return;

    if (g_saved_flags != -1)
        fcntl(STDIN_FILENO, F_SETFL, g_saved_flags);
    if (g_saved_termios_ok)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);

    g_saved_flags = -1;
    g_saved_termios_ok = 0;
}
