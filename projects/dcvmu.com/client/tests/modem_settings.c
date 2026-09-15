/* Exercise modem setup/failure paths on SH-4 with controlled hardware replies.
   KOS threads/semaphores are real; no flash writes, dialing or VMU access. */
#define flashrom_get_pw_ispcfg test_pw
#define flashrom_get_ispcfg test_dp
#define modem_init test_detect
#define modem_shutdown test_hangup
#define ppp_init test_ppp_init
#define ppp_shutdown test_ppp_shutdown
#define ppp_modem_init test_dial
#define ppp_set_login test_login
#define ppp_connect test_connect
#include "../modem.c"
#include <assert.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);
static flashrom_ispcfg_t pw,dp;
static int detect_result=1,dial_result,connect_result;
static int dial_calls,ppp_calls,shutdown_calls,hangup_calls,cancel,cancel_after_dial;
static int ppp_init_result,login_result;
static char used_number[64],used_login[64],used_password[64],status[128];
static netif_t fake={.name="ppp"};

int test_pw(flashrom_ispcfg_t *out) {*out=pw;return 0;}
int test_dp(flashrom_ispcfg_t *out) {*out=dp;return 0;}
int test_detect(void) {return detect_result;}
void test_hangup(void) {++hangup_calls;}
int test_ppp_init(void) {++ppp_calls;return ppp_init_result;}
int test_ppp_shutdown(void) {++shutdown_calls;net_default_dev=NULL;return 0;}
int test_login(const char *u,const char *p) {
    strcpy(used_login,u);strcpy(used_password,p);return login_result;
}
int test_dial(const char *n,int blind,int *rate) {
    ++dial_calls;strcpy(used_number,n);assert(blind==!!(pw.flags&FLASHROM_ISP_BLIND_DIAL));
    if(cancel_after_dial)thd_sleep(250);
    *rate=33600;return dial_result;
}
int test_connect(void) {net_default_dev=&fake;return connect_result;}
void client_status(const char *s) {snprintf(status,sizeof(status),"%s",s);}
int client_connect_update(const char *s) {(void)s;return cancel || (cancel_after_dial && dial_calls);}

static void reset(void) {
    service_modem_shutdown();
    memset(&pw,0,sizeof(pw));memset(&dp,0,sizeof(dp));
    memset(fake.ip_addr,0,4);memset(fake.dns,0,4);
    fake.ip_addr[0]=192;fake.ip_addr[3]=2;fake.dns[0]=192;fake.dns[3]=1;
    net_default_dev=NULL;detect_result=1;dial_result=connect_result=cancel=0;
    cancel_after_dial=ppp_init_result=login_result=0;
    dial_calls=ppp_calls=shutdown_calls=hangup_calls=0;
}

int main(void) {
    /* INIT_NET must initialize sockets with zero Ethernet devices. */
    int sock=socket(AF_INET,SOCK_STREAM,0);assert(sock>=0);close(sock);
    reset();
    assert(service_modem_init()==0);
    assert(!strcmp(used_number,"555") && !strcmp(used_login,"dream") && !strcmp(used_password,"cast"));
    service_modem_shutdown();service_modem_shutdown();
    assert(shutdown_calls==1 && hangup_calls==1 && !net_default_dev);

    reset();
    pw.valid_fields=FLASHROM_ISP_PHONE1|FLASHROM_ISP_PPP_USER|FLASHROM_ISP_PPP_PASS;
    strcpy(pw.phone1,"5 (5)-5");strcpy(pw.ppp_login,"saved-user");strcpy(pw.ppp_passwd,"saved-pass");
    pw.flags=FLASHROM_ISP_BLIND_DIAL;
    assert(service_modem_init()==0);
    assert(!strcmp(used_number,"555") && !strcmp(used_login,"saved-user") && !strcmp(used_password,"saved-pass"));

    reset();
    pw.valid_fields=FLASHROM_ISP_PPP_USER;strcpy(pw.ppp_login,"wrong-profile");
    dp.valid_fields=FLASHROM_ISP_PHONE1|FLASHROM_ISP_PPP_USER|FLASHROM_ISP_DNS;
    strcpy(dp.phone1,"123");strcpy(dp.ppp_login,"passport");dp.dns[1][0]=8;dp.dns[1][3]=8;
    memset(fake.dns,0,4);
    assert(service_modem_init()==0);
    assert(!strcmp(used_number,"123") && !strcmp(used_login,"passport") && fake.dns[0]==8);

    reset();detect_result=0;
    assert(service_modem_init()<0 && !ppp_calls && !dial_calls && !hangup_calls);
    reset();cancel=1;
    assert(service_modem_init()<0 && !ppp_calls && !dial_calls && !hangup_calls);
    reset();cancel_after_dial=1;
    assert(service_modem_init()<0 && dial_calls==1 && shutdown_calls==1 && hangup_calls==1);
    reset();ppp_init_result=-1;
    assert(service_modem_init()<0 && !dial_calls && !shutdown_calls && hangup_calls==1);
    reset();login_result=-1;
    assert(service_modem_init()<0 && !dial_calls && shutdown_calls==1 && hangup_calls==1);
    reset();dial_result=-2;
    assert(service_modem_init()<0 && shutdown_calls==1 && hangup_calls==1 && strstr(status,"dial tone"));
    reset();connect_result=-1;
    assert(service_modem_init()<0 && shutdown_calls==1 && !net_default_dev);
    reset();memset(fake.dns,0,4);
    assert(service_modem_init()<0 && shutdown_calls==1 && strstr(status,"DNS"));
    reset();memset(fake.ip_addr,0,4);
    assert(service_modem_init()<0 && shutdown_calls==1 && strstr(status,"IP address"));
    reset();pw.valid_fields=FLASHROM_ISP_PHONE1;strcpy(pw.phone1,"5,55");
    assert(service_modem_init()<0 && !dial_calls && !ppp_calls);
    reset();pw.valid_fields=FLASHROM_ISP_PHONE1;strcpy(pw.phone1,"555");pw.flags=FLASHROM_ISP_PULSE_DIAL;
    assert(service_modem_init()<0 && !dial_calls && !ppp_calls);
    reset();
    printf("MODEM SETTINGS AND FAILURE CHECKS PASSED\n");
    while(1)thd_sleep(1000);
}
