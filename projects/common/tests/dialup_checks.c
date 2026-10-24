/* Exercise the shared dialer's settings, failure, cancel and drop paths on
   SH-4 with controlled hardware replies. KOS threads are real; nothing
   dials, and flash and VMUs are never written. */
#define flashrom_get_pw_ispcfg test_pw
#define flashrom_get_ispcfg test_dp
#define modem_init test_detect
#define modem_shutdown test_hangup
#define modem_is_connected test_carrier
#define ppp_init test_ppp_init
#define ppp_shutdown test_ppp_shutdown
#define ppp_add_protocol test_add_protocol
#define ppp_modem_init test_dial
#define ppp_set_login test_login
#define ppp_connect test_connect
#include "../dialup.c"
#include <assert.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);
static flashrom_ispcfg_t pw,dp;
static int detect_result=1,dial_result,connect_result,carrier=1;
static int dial_calls,connect_calls,ppp_calls,shutdown_calls,hangup_calls,protocol_calls,progress_calls;
static int ppp_init_result,login_result,dial_delay_ms,cancel_now,cancel_after_dial;
static char used_number[64],used_login[64],used_password[64];
static netif_t fake={.name="ppp"};
static ppp_protocol_t *monitor;

int test_pw(flashrom_ispcfg_t *out) {*out=pw;return 0;}
int test_dp(flashrom_ispcfg_t *out) {*out=dp;return 0;}
int test_detect(void) {return detect_result;}
void test_hangup(void) {++hangup_calls;}
int test_carrier(void) {return carrier;}
int test_ppp_init(void) {++ppp_calls;return ppp_init_result;}
int test_ppp_shutdown(void) {++shutdown_calls;net_default_dev=NULL;monitor=NULL;return 0;}
int test_add_protocol(ppp_protocol_t *p) {++protocol_calls;monitor=p;return 0;}
int test_login(const char *u,const char *p) {
    strcpy(used_login,u);strcpy(used_password,p);return login_result;
}
int test_dial(const char *n,int blind,int *rate) {
    ++dial_calls;strcpy(used_number,n);assert(blind==!!(pw.flags&FLASHROM_ISP_BLIND_DIAL));
    if(dial_delay_ms)thd_sleep(dial_delay_ms);
    *rate=33600;return dial_result;
}
int test_connect(void) {++connect_calls;net_default_dev=&fake;return connect_result;}
static int progress(dialup_stage_t stage) {
    (void)stage;++progress_calls;
    return cancel_now || (cancel_after_dial && dial_calls);
}
static dialup_error_t run(void) {return dialup_connect(NULL,DIALUP_REQUIRE_DNS,progress);}
static void wait_idle(void) {while(dialup_busy())thd_sleep(10);}

static void reset(void) {
    dialup_hang_up();
    memset(&pw,0,sizeof(pw));memset(&dp,0,sizeof(dp));
    memset(fake.ip_addr,0,4);memset(fake.dns,0,4);
    fake.ip_addr[0]=192;fake.ip_addr[3]=2;fake.dns[0]=192;fake.dns[3]=1;
    net_default_dev=NULL;detect_result=carrier=1;dial_result=connect_result=0;
    cancel_now=cancel_after_dial=ppp_init_result=login_result=dial_delay_ms=0;
    dial_calls=connect_calls=ppp_calls=shutdown_calls=hangup_calls=protocol_calls=progress_calls=0;
}

int main(void) {
    /* INIT_NET must initialize sockets with zero Ethernet devices. */
    int sock=socket(AF_INET,SOCK_STREAM,0);assert(sock>=0);close(sock);

    /* DreamPi defaults, then a clean double hang-up. */
    reset();
    assert(run()==DIALUP_OK && dialup_stage()==DIALUP_READY && dialup_online());
    assert(!strcmp(used_number,"555") && !strcmp(used_login,"dream") && !strcmp(used_password,"cast"));
    assert(dialup_rate()==33600 && protocol_calls==1 && progress_calls>=1);
    dialup_hang_up();dialup_hang_up();
    assert(shutdown_calls==1 && hangup_calls==1 && !net_default_dev && dialup_stage()==DIALUP_IDLE);

    /* A saved PlanetWeb profile, punctuation stripped from the number. */
    reset();
    pw.valid_fields=FLASHROM_ISP_PHONE1|FLASHROM_ISP_PPP_USER|FLASHROM_ISP_PPP_PASS;
    strcpy(pw.phone1,"5 (5)-5");strcpy(pw.ppp_login,"saved-user");strcpy(pw.ppp_passwd,"saved-pass");
    pw.flags=FLASHROM_ISP_BLIND_DIAL;
    assert(run()==DIALUP_OK);
    assert(!strcmp(used_number,"555") && !strcmp(used_login,"saved-user") && !strcmp(used_password,"saved-pass"));

    /* PlanetWeb without a number falls back to the whole DreamPassport
       profile, whose second DNS server fills in for PPP's missing one. */
    reset();
    pw.valid_fields=FLASHROM_ISP_PPP_USER;strcpy(pw.ppp_login,"wrong-profile");
    dp.valid_fields=FLASHROM_ISP_PHONE1|FLASHROM_ISP_PPP_USER|FLASHROM_ISP_DNS;
    strcpy(dp.phone1,"123");strcpy(dp.ppp_login,"passport");dp.dns[1][0]=8;dp.dns[1][3]=8;
    memset(fake.dns,0,4);
    assert(run()==DIALUP_OK);
    assert(!strcmp(used_number,"123") && !strcmp(used_login,"passport") && fake.dns[0]==8);

    reset();detect_result=0;
    assert(run()==DIALUP_ERR_NO_MODEM && !ppp_calls && !dial_calls && !hangup_calls);
    assert(dialup_stage()==DIALUP_FAILED && !dialup_retryable(dialup_error()));
    /* Canceled before the modem is touched. */
    reset();cancel_now=1;
    assert(run()==DIALUP_ERR_CANCELED && progress_calls==1 && !ppp_calls && !dial_calls && !hangup_calls);
    /* Canceled while dialing: the dial finishes, then everything is released. */
    reset();cancel_after_dial=1;dial_delay_ms=250;
    assert(run()==DIALUP_ERR_CANCELED && dial_calls==1 && !connect_calls && shutdown_calls==1 && hangup_calls==1);
    reset();ppp_init_result=-1;
    assert(run()==DIALUP_ERR_PPP_SETUP && !dial_calls && !shutdown_calls && hangup_calls==1);
    reset();login_result=-1;
    assert(run()==DIALUP_ERR_PPP_SETUP && !dial_calls && shutdown_calls==1 && hangup_calls==1);
    reset();dial_result=-2;
    assert(run()==DIALUP_ERR_NO_DIAL_TONE && shutdown_calls==1 && hangup_calls==1);
    assert(dialup_retryable(dialup_error()));
    reset();dial_result=-3;
    assert(run()==DIALUP_ERR_DIAL);
    reset();dial_result=-4;
    assert(run()==DIALUP_ERR_NO_ANSWER && dialup_retryable(dialup_error()));
    reset();dial_result=-1;
    assert(run()==DIALUP_ERR_NO_MODEM);
    reset();connect_result=-1;
    assert(run()==DIALUP_ERR_PPP && shutdown_calls==1 && !net_default_dev);
    reset();memset(fake.dns,0,4);
    assert(run()==DIALUP_ERR_NO_DNS && shutdown_calls==1);
    /* Without DIALUP_REQUIRE_DNS a missing DNS server is the caller's problem. */
    reset();memset(fake.dns,0,4);
    assert(dialup_connect(NULL,0,NULL)==DIALUP_OK && dialup_online());
    reset();memset(fake.ip_addr,0,4);
    assert(run()==DIALUP_ERR_NO_ADDRESS && shutdown_calls==1);
    /* KOS dials tone digits only. */
    reset();pw.valid_fields=FLASHROM_ISP_PHONE1;strcpy(pw.phone1,"5,55");
    assert(run()==DIALUP_ERR_SETTINGS && !dial_calls && !ppp_calls);
    reset();pw.valid_fields=FLASHROM_ISP_PHONE1;strcpy(pw.phone1,"555");pw.flags=FLASHROM_ISP_PULSE_DIAL;
    assert(run()==DIALUP_ERR_SETTINGS && !dial_calls && !ppp_calls);

    /* The worker thread, polled as a frame loop would. */
    reset();dial_delay_ms=200;
    assert(dialup_start(NULL,0)==0 && dialup_busy() && dialup_start(NULL,0)<0);
    assert(dialup_connect(NULL,0,NULL)==DIALUP_ERR_THREAD);
    wait_idle();
    assert(dialup_stage()==DIALUP_READY && dialup_error()==DIALUP_OK && dialup_online());
    /* PPP leaving its network phase is a drop, until a redial. */
    assert(monitor && !dialup_dropped());
    monitor->enter_phase(monitor,PPP_PHASE_NETWORK,PPP_PHASE_TERMINATE);
    assert(dialup_dropped() && !dialup_online());
    assert(dialup_start(NULL,0)==0);wait_idle();
    assert(dialup_stage()==DIALUP_READY && !dialup_dropped() && shutdown_calls==1 && hangup_calls==1);
    /* So is losing carrier. */
    carrier=0;
    assert(dialup_dropped() && !dialup_online());
    /* Cancel mid-dial: the worker stops after the dial returns. */
    reset();dial_delay_ms=300;
    assert(dialup_start(NULL,0)==0);thd_sleep(50);dialup_cancel();wait_idle();
    assert(dialup_stage()==DIALUP_FAILED && dialup_error()==DIALUP_ERR_CANCELED);
    assert(dial_calls==1 && !connect_calls && shutdown_calls==1 && hangup_calls==1);
    /* Hanging up waits for a worker still dialing. */
    reset();dial_delay_ms=300;
    assert(dialup_start(NULL,0)==0);thd_sleep(50);dialup_hang_up();
    assert(!dialup_busy() && dialup_stage()==DIALUP_IDLE && dial_calls==1 && hangup_calls==1);
    /* Progress keeps coming during a slow step. */
    reset();dial_delay_ms=500;
    assert(run()==DIALUP_OK && progress_calls>=4);
    reset();
    printf("DIALUP CHECKS PASSED\n");
    while(1)thd_sleep(1000);
}
