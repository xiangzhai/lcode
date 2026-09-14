#ifndef LCODE_INTERRUPT_H
#define LCODE_INTERRUPT_H

void interrupt_clear(void);
int interrupt_pending(void);

void interrupt_watch_begin(void);
int interrupt_poll(void);
void interrupt_watch_end(void);

#endif
