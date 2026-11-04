#ifndef RECEIVER_H
#define RECEIVER_H
int receiver_init(const char *speaker);
void receiver_shutdown(void);
void receiver_status(void);
void receiver_command(const char *command);
#endif
