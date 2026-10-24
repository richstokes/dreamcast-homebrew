/* Dial-up for the browser: the shared dialer runs on its own thread while
   the frame loop keeps drawing and reading input, and polls it here. */

#include "browser.h"
#include "../../common/dialup.h"

/* Set from a start until the poll that reports how it ended. */
static int started;

static const char *failure_text(dialup_error_t error) {
    switch(error) {
        case DIALUP_ERR_NO_MODEM: return "No network adapter or modem was found";
        case DIALUP_ERR_SETTINGS: return "The saved dial settings need tone dialing and a plain number";
        case DIALUP_ERR_PPP_SETUP: return "The modem could not be prepared";
        case DIALUP_ERR_NO_DIAL_TONE: return "No dial tone: check the DreamPi and the phone cable";
        case DIALUP_ERR_DIAL: return "The number could not be dialed";
        case DIALUP_ERR_NO_ANSWER: return "Nothing answered the call: check the DreamPi";
        case DIALUP_ERR_PPP: return "The connection refused the saved login";
        case DIALUP_ERR_NO_ADDRESS: return "The connection did not supply an address";
        case DIALUP_ERR_NO_DNS: return "The connection did not supply a DNS server";
        case DIALUP_ERR_CANCELED: return "Connection canceled";
        default: return "The modem could not be started";
    }
}

int modem_connect_start(void) {
    if(started || dialup_start("browser", DIALUP_REQUIRE_DNS) < 0) return -1;
    started = 1;
    return 0;
}

const char *modem_connect_status(void) {
    switch(dialup_stage()) {
        case DIALUP_DIALING: return "dialing (to 60 s)";
        case DIALUP_NEGOTIATING: return "signing in";
        case DIALUP_READY: return "connected";
        case DIALUP_FAILED: return failure_text(dialup_error());
        default: return "finding modem";
    }
}

int modem_connect_poll(const char **status) {
    *status = modem_connect_status();
    if(!started || dialup_busy()) return 0;
    started = 0;
    return dialup_stage() == DIALUP_READY ? 1 : -1;
}

void modem_connect_cancel(void) {
    dialup_cancel();
}

int modem_in_use(void) {
    return dialup_stage() == DIALUP_READY;
}

int modem_link_up(void) {
    return dialup_online();
}

void modem_disconnect_link(void) {
    dialup_hang_up();
    started = 0;
}
