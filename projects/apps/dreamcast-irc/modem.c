/* Dial-up for the IRC client: the shared dialer, with the status page kept
   drawn while each blocking KOS step runs. */
#include "modem.h"
#include "../../common/dialup.h"

static int progress(dialup_stage_t stage) {
    return irc_modem_progress(
        stage == DIALUP_DIALING ? "Dialing DreamPi... Allow up to 65 seconds." :
        stage == DIALUP_NEGOTIATING ? "Establishing PPP connection to DreamPi..." :
        "Checking for a Dreamcast modem...");
}

static const char *failure_text(dialup_error_t error) {
    switch(error) {
        case DIALUP_ERR_NO_MODEM: return "No modem found. Attach a modem, BBA or W5500 and restart.";
        case DIALUP_ERR_SETTINGS: return "Unsupported dial settings. Configure tone dialing for DreamPi.";
        case DIALUP_ERR_NO_DIAL_TONE: return "No dial tone. Check DreamPi and the phone cable.";
        case DIALUP_ERR_DIAL: return "Could not dial. Check saved ISP settings.";
        case DIALUP_ERR_NO_ANSWER: return "Modem did not connect. Check DreamPi and use /reconnect.";
        case DIALUP_ERR_PPP: return "PPP failed. Check saved ISP login and DreamPi.";
        case DIALUP_ERR_NO_ADDRESS: return "PPP did not supply an IP address. Use /reconnect to retry.";
        case DIALUP_ERR_NO_DNS: return "No modem DNS server. Check DreamPi or saved ISP DNS.";
        case DIALUP_ERR_CANCELED: return "Modem connection canceled.";
        case DIALUP_ERR_THREAD: return "Could not start modem worker. Use /reconnect to retry.";
        default: return "Modem startup failed. Use /reconnect to retry.";
    }
}

void irc_modem_shutdown(void) {
    dialup_hang_up();
}

int irc_modem_connect(void) {
    dialup_error_t error = dialup_connect("Dreamcast IRC", DIALUP_REQUIRE_DNS, progress);

    if(error == DIALUP_OK)
        return 0;
    irc_modem_status(failure_text(error));
    return -1;
}

int irc_modem_active(void) {
    return dialup_stage() == DIALUP_READY;
}

int irc_modem_online(void) {
    return dialup_online();
}
