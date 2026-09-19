#ifndef DCIRC_MODEM_H
#define DCIRC_MODEM_H

int irc_modem_connect(void);
int irc_modem_active(void);
int irc_modem_online(void);
void irc_modem_shutdown(void);
/* UI callbacks run only on the main thread. Nonzero requests cancellation
   after the current blocking KOS dial/PPP step completes. */
int irc_modem_progress(const char *message);
void irc_modem_status(const char *message);

#endif
