/* Included only by the isolated local-server integration build. */
#include <dc/modem/modem.h>
static bool test_history_contains(const char *text) {
    for(int p=0;p<irc_page_count;++p)
        for(int h=0;h<HISTORY_LINES;++h)
            if(strstr(pages[p].history[h].text,text))return true;
    return false;
}

static void test_command(const char *command) {
    snprintf(input_text,sizeof(input_text),"%s",command);
    input_length=strlen(input_text);
    submit_input();
}

static void network_self_test(void) {
    static int round=1;
    static bool sent;
    char ready[32],reply[32],message[32];
    int last_round=modem_selected?4:3;
    if(round>last_round)return;
    snprintf(ready,sizeof(ready),"fixture-ready-%d",round);
    snprintf(reply,sizeof(reply),"fixture-ok-%d",round);
    if(connection_state==CONN_ONLINE && irc_page_count>1 && pages[1].joined &&
       test_history_contains(ready) && !sent) {
        active_page=1;
        snprintf(message,sizeof(message),"fixture-out-%d",round);
        test_command(message);sent=true;
    }
    if(sent && test_history_contains(reply)) {
        printf("IRC NETWORK TEST ROUND %d PASS\n",round);
        ++round;sent=false;
        if(round==3)test_command("/reconnect");
        if(round==4 && modem_selected) {
            /* Drop the actual emulated carrier and exercise automatic redial. */
            modem_disconnect();
        }
        if(round>last_round) {
            printf("IRC NETWORK TEST PASSED\n");
            test_command("/quit Fixture complete");
        }
    }
}
