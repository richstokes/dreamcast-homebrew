#include "client.h"
#include <kos.h>
#include <kos/sem.h>
#include <dc/flashrom.h>
#include <dc/modem/modem.h>
#include <ppp/ppp.h>
#include <stdio.h>
#include <string.h>

static int modem_owned, ppp_owned;

/* KOS dial/PPP calls block. Keep drawing and reading input on the main thread;
   cancellation is deferred until the current SDK call returns. Never destroy
   a thread while it holds the modem or PPP library's locks. */
typedef struct {
    semaphore_t done;
    int step, result, blind, rate;
    const char *number;
} connect_job;

static void *connect_worker(void *arg) {
    connect_job *job=arg;
    if(job->step==0) job->result=modem_init()?0:-1;
    else if(job->step==1) job->result=ppp_modem_init(job->number,job->blind,&job->rate);
    else job->result=ppp_connect();
    sem_signal(&job->done);
    return NULL;
}

static int run_step(connect_job *job,const char *message) {
    int canceled=client_connect_update(message);
    if(canceled) return -10;
    sem_init(&job->done,0);
    kthread_t *worker=thd_create(0,connect_worker,job);
    if(!worker) { sem_destroy(&job->done);return -11; }
    while(sem_wait_timed(&job->done,100)<0)
        canceled|=client_connect_update(message);
    thd_join(worker,NULL);
    sem_destroy(&job->done);
    return canceled?-10:job->result;
}

static int has_phone(const flashrom_ispcfg_t *cfg) {
    return (cfg->valid_fields&FLASHROM_ISP_PHONE1) && cfg->phone1[0];
}

/* Use a whole profile, rather than combining credentials from two browsers.
   The fallback matches KOS's DreamPi example. Flash is strictly read-only. */
static void load_settings(flashrom_ispcfg_t *cfg) {
    if(flashrom_get_pw_ispcfg(cfg)==0 && has_phone(cfg)) {
        printf("dcvmu: modem using saved PlanetWeb ISP profile\n");return;
    }
    if(flashrom_get_ispcfg(cfg)==0 && has_phone(cfg)) {
        printf("dcvmu: modem using saved DreamPassport ISP profile\n");return;
    }
    memset(cfg,0,sizeof(*cfg));
    strcpy(cfg->phone1,"555");
    printf("dcvmu: modem using DreamPi defaults (no saved dial profile)\n");
}

static int dial_number(const flashrom_ispcfg_t *cfg,char number[64]) {
    char raw[64];
    /* KOS supports DTMF only, not AT commands, pause/wait or pulse dialing.
       Refuse unsupported dial syntax instead of silently dialing a different
       number. DreamPi needs no outside-line/call-waiting/long-distance prefix. */
    if(cfg->flags&FLASHROM_ISP_PULSE_DIAL) return -1;
    snprintf(raw,sizeof(raw),"%.*s%.*s",
             (cfg->flags&FLASHROM_ISP_DIAL_AREACODE)?3:0,cfg->p1_areacode,
             (int)sizeof(cfg->phone1),cfg->phone1);
    size_t n=0;
    for(size_t i=0;raw[i];++i) {
        if(strchr("0123456789*#ABCD",raw[i])) number[n++]=raw[i];
        else if(!strchr(" -().",raw[i])) return -1;
    }
    number[n]=0;
    return n?0:-1;
}

void service_modem_shutdown(void) {
    /* Requires patches/kos-ppp-lifecycle.patch: stop/join PPP before freeing
       the modem buffers. Stock KOS 2.3 ppp_shutdown does not stop its worker. */
    if(ppp_owned) { ppp_shutdown();ppp_owned=0; }
    if(modem_owned) { modem_shutdown();modem_owned=0; }
}

int service_modem_init(void) {
    flashrom_ispcfg_t cfg;
    char number[64],login[30],password[21];
    connect_job job={.result=-1};
    const char *failure="Modem startup failed. Restart to retry.";
    int result=run_step(&job,"Checking for a Dreamcast modem...");
    /* Detection may have succeeded even if cancellation was requested. */
    modem_owned=job.result==0;
    if(result<0) {
        failure="No modem found. Attach a modem or BBA and restart.";
        goto failed;
    }
    load_settings(&cfg);
    if(dial_number(&cfg,number)<0) {
        failure="Unsupported dial settings. Configure tone dialing for DreamPi.";
        memset(&cfg,0,sizeof(cfg));goto failed;
    }
    snprintf(login,sizeof(login),"%.*s",(int)sizeof(cfg.ppp_login),
             (cfg.valid_fields&FLASHROM_ISP_PPP_USER)?cfg.ppp_login:"dream");
    snprintf(password,sizeof(password),"%.*s",(int)sizeof(cfg.ppp_passwd),
             (cfg.valid_fields&FLASHROM_ISP_PPP_PASS)?cfg.ppp_passwd:"cast");
    result=ppp_init();
    if(result==0) {
        ppp_owned=1;
        result=ppp_set_login(login,password);
    }
    memset(login,0,sizeof(login));memset(password,0,sizeof(password));
    if(!ppp_owned || result<0) { memset(&cfg,0,sizeof(cfg));goto failed; }
    job.step=1;job.number=number;job.blind=!!(cfg.flags&FLASHROM_ISP_BLIND_DIAL);
    /* Keep only the optional DNS fallback, not the flash profile's passwords. */
    unsigned char dns[4]={0};
    if(cfg.valid_fields&FLASHROM_ISP_DNS) {
        memcpy(dns,cfg.dns[0],4);
        if(!(dns[0]|dns[1]|dns[2]|dns[3]))memcpy(dns,cfg.dns[1],4);
    }
    memset(&cfg,0,sizeof(cfg));
    result=run_step(&job,"Dialing DreamPi... Allow up to 65 seconds.");
    if(result<0) {
        failure=result==-2?"No dial tone. Check DreamPi and the phone cable.":
                result==-3?"Could not dial. Check saved ISP settings.":
                "Modem did not connect. Check DreamPi and restart.";
        goto failed;
    }
    printf("dcvmu: modem carrier %d bps; negotiating PPP\n",job.rate);
    job.step=2;
    result=run_step(&job,"Establishing PPP connection to DreamPi...");
    if(result<0) { failure="PPP failed. Check saved ISP login and DreamPi.";goto failed; }
    if(!net_default_dev || strcmp(net_default_dev->name,"ppp") ||
       !(net_default_dev->ip_addr[0]|net_default_dev->ip_addr[1]|
         net_default_dev->ip_addr[2]|net_default_dev->ip_addr[3])) {
        failure="PPP did not supply an IP address. Restart to retry.";goto failed;
    }
    netif_t *dev=net_default_dev;
    if(!(dev->dns[0]|dev->dns[1]|dev->dns[2]|dev->dns[3]))memcpy(dev->dns,dns,4);
    if(!(dev->dns[0]|dev->dns[1]|dev->dns[2]|dev->dns[3])) {
        failure="No modem DNS server. Check DreamPi or saved ISP DNS.";goto failed;
    }
    printf("dcvmu: PPP ready, IP %u.%u.%u.%u DNS %u.%u.%u.%u\n",
           dev->ip_addr[0],dev->ip_addr[1],dev->ip_addr[2],dev->ip_addr[3],
           dev->dns[0],dev->dns[1],dev->dns[2],dev->dns[3]);
    return 0;
failed:
    service_modem_shutdown();
    if(result==-10)failure="Modem connection canceled. Restart to reconnect.";
    if(result==-11)failure="Could not start modem worker. Restart to retry.";
    client_status(failure);
    printf("dcvmu: %s (SDK result %d)\n",failure,result);
    return -1;
}
