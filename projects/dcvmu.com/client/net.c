#include "client.h"
#include <kos.h>
#include <mbedtls/sha256.h>
#include <curl/curl.h>
#include <dc/asic.h>
#include <kos/irq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SERVICE "https://dcvmu.com"
static kthread_t *poll_thread;
static CURL *http_client;
static volatile int polling;
static int gate_bba;
static int using_modem, curl_ready;
static char response[16384];
static size_t received;
static char error[CURL_ERROR_SIZE];

static void *poll_worker(void *unused) {
    (void)unused;
    while(polling) {
        if(net_default_dev && net_default_dev->if_rx_poll)
            net_default_dev->if_rx_poll(net_default_dev);
        thd_sleep(4);
    }
    return NULL;
}
static size_t receive(char *data, size_t size, size_t count, void *unused) {
    size_t bytes = size * count;
    (void)unused;
    if(bytes >= sizeof(response) - received) return 0;
    memcpy(response + received, data, bytes);
    received += bytes;
    response[received] = 0;
    return bytes;
}
static int progress(void *unused, curl_off_t total, curl_off_t done,
                    curl_off_t upload_total, curl_off_t uploaded) {
    (void)unused;
    return transfer_update((uint64_t)(done + uploaded), (uint64_t)(total + upload_total));
}
int service_net_init(void) {
    /* INIT_NET already selects Ethernet when present. Never probe/reset the
       modem on that path: the two adapters share the expansion interface. */
    if(!net_default_dev) {
        if(service_modem_init()<0) return -1;
        using_modem=1;
    }
    if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        client_status("HTTPS startup failed. Restart to retry.");
        service_net_shutdown();return -1;
    }
    curl_ready=1;
    gate_bba = net_default_dev && !strcmp(net_default_dev->name, "bba");
    if(gate_bba) {
        uint32_t irq = irq_disable();
        asic_evt_disable(ASIC_EVT_EXP_PCI, ASIC_IRQ_DEFAULT);
        irq_restore(irq);
        polling = 1;
        poll_thread = thd_create(0, poll_worker, NULL);
        if(!poll_thread) {
            uint32_t restore_irq=irq_disable();
            asic_evt_enable(ASIC_EVT_EXP_PCI, ASIC_IRQ_DEFAULT);
            irq_restore(restore_irq);gate_bba=0;
            client_status("BBA receive startup failed. Restart to retry.");
            service_net_shutdown();return -1;
        }
        printf("dcvmu: BBA receive polling enabled\n");
    }
    return 0;
}
void service_net_shutdown(void) {
    polling=0;
    if(poll_thread) { thd_join(poll_thread, NULL); poll_thread=NULL; }
    if(http_client) { curl_easy_cleanup(http_client); http_client=NULL; }
    if(curl_ready) { curl_global_cleanup(); curl_ready=0; }
    /* Preserve the existing BBA shutdown behavior: keep PCI IRQs gated until
       system shutdown. Re-enabling them here reintroduces Flycast IRQ re-entry
       while late TCP/Maple traffic is still arriving at the thank-you screen. */
    gate_bba=0;
    if(using_modem) { service_modem_shutdown(); using_modem=0; }
}
#ifdef DCVMU_SELF_TEST
static int trace(CURL *handle, curl_infotype type, char *data, size_t size, void *unused) {
    (void)handle;(void)unused;
    if(type==CURLINFO_TEXT) {
        /* Never emit HTTP headers or request/response bodies (they contain credentials). */
        printf("dcvmu TLS: %.*s",(int)size,data);
    }
    return 0;
}
#endif
static CURL *request_new(const char *path) {
    CURL *curl;
    if(!http_client) http_client=curl_easy_init();
    curl=http_client;
    if(curl)curl_easy_reset(curl);
    char url[640];
    if(!curl) { client_status("Could not allocate HTTPS client."); return NULL; }
    const char *origin=SERVICE;
#ifdef DCVMU_DOWNLOAD_TEST
    char test_origin[160];int local_test=0;
    FILE *test_file=fopen("/rd/test-service.txt","r");
    if(test_file) {
        if(fgets(test_origin,sizeof(test_origin),test_file)) {
            test_origin[strcspn(test_origin,"\r\n")]=0;origin=test_origin;local_test=1;
        }
        fclose(test_file);
    }
#endif
    snprintf(url, sizeof(url), "%s%s", origin, path);
    received = 0; response[0] = 0; error[0] = 0;
#ifdef DCVMU_SELF_TEST
    curl_easy_setopt(curl,CURLOPT_VERBOSE,1L);
    curl_easy_setopt(curl,CURLOPT_DEBUGFUNCTION,trace);
#endif
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/rd/cacert.pem");
#ifdef DCVMU_DOWNLOAD_TEST
    if(local_test)curl_easy_setopt(curl,CURLOPT_CAINFO,"/rd/test-ca.pem");
#endif
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, using_modem?60000L:15000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, using_modem?300000L:60000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 4096L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, 4096L);
    curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)65536);
    /* The small upload buffer bounds bursts without rate-limiter sleeps in KOS. */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DCVMU/1.0 (KallistiOS)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    return curl;
}
static long perform(CURL *curl) {
    long status = 0;
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if(result != CURLE_OK) {
        client_status(result == CURLE_ABORTED_BY_CALLBACK ? "Canceled. Check account before retrying." :
                      result == CURLE_OPERATION_TIMEDOUT ? "Request timed out. Check account before retrying." :
                      "HTTPS failed. Check connection and console clock.");
        printf("dcvmu: HTTPS transport error: %s\n", error[0] ? error : curl_easy_strerror(result));
        return -1;
    }
    printf("dcvmu: HTTP %ld (%u response bytes)\n", status, (unsigned)received);
    if(status >= 400 && status != 409) client_status(response);
    return status;
}
int service_login(const char *username, const char *password, char *token, size_t capacity) {
    CURL *curl = request_new("/api/v1/login");
    char *u, *p, *body;
    long status;
    if(!curl) return -1;
    u = curl_easy_escape(curl, username, 0);
    p = curl_easy_escape(curl, password, 0);
    body = malloc((u ? strlen(u) : 0) + (p ? strlen(p) : 0) + 48);
    if(!u || !p || !body) { curl_free(u); curl_free(p); free(body); curl_easy_reset(curl); return -1; }
    sprintf(body, "username=%s&password=%s&remember=1", u, p);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    status = perform(curl);
    memset(body, 0, strlen(body)); memset(p, 0, strlen(p));
    free(body); curl_free(u); curl_free(p); curl_easy_reset(curl);
    if(status != 200) return -1;
    response[strcspn(response, "\r\n")] = 0;
    if(strlen(response) < 32 || strlen(response) >= capacity) return -1;
    snprintf(token, capacity, "%s", response);
    memset(response, 0, sizeof(response));
    return 0;
}
static void part(curl_mime *mime, const char *key, const char *value) {
    curl_mimepart *p = curl_mime_addpart(mime);
    curl_mime_name(p, key); curl_mime_data(p, value, CURL_ZERO_TERMINATED);
}
int service_upload(const char *token, const char *name, const char *filename,
                   const char *game, const char *notes, int private_save,
                   const void *data, size_t size, int header_offset, const char *mode, int save_id, int *revision) {
    if(auth_is_save(filename,data,size)){client_status("Login saves cannot be uploaded.");return -1;}
    CURL *curl = request_new("/api/v1/saves");
    curl_mime *mime;
    curl_mimepart *file;
    struct curl_slist *headers = NULL;
    char auth[128], version[24];
    long status;
    if(!curl) return -1;
    /* Uploads follow user interaction and may outlive an idle TCP connection.
       Use a new connection rather than risking a stalled POST on a stale one.
       Never retry automatically: the server may already have stored the save. */
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    mime = curl_mime_init(curl);
    if(!mime) { curl_easy_reset(curl); return -1; }
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Expect:");
    part(mime, "name", name); part(mime, "filename", filename);
    part(mime, "match", "filename");
    if(save_id>0){snprintf(version,sizeof(version),"%d",save_id);part(mime,"save_id",version);}
    part(mime, "game", game); part(mime, "notes", notes);
    part(mime, "private", private_save ? "1" : "0"); part(mime, "mode", mode);
    snprintf(version, sizeof(version), "%d", header_offset); part(mime, "header_offset", version);
    snprintf(version, sizeof(version), "%d", *revision); part(mime, "revision", version);
    file = curl_mime_addpart(mime); curl_mime_name(file, "save");
    curl_mime_filename(file, "save.vms"); curl_mime_type(file, "application/octet-stream");
    curl_mime_data(file, data, size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    status = perform(curl);
    curl_mime_free(mime); curl_slist_free_all(headers); curl_easy_reset(curl);
    memset(auth, 0, sizeof(auth));
    if(status == 409 && !strcmp(response, "MATCHES\n"))return 2;
    if(status == 409 && !strncmp(response, "CONFLICT\n", 9)) {
        *revision = atoi(response + 9); return 1;
    }
    if(status == 201) { client_status("Uploaded. View your save at dcvmu.com."); return 0; }
    if(status == 409) client_status("Save changed. Go back and upload again.");
    if(status == 401) client_status("Login expired. Restart and log in again.");
    return -1;
}
void service_logout(const char *token) {
    CURL *curl = request_new("/api/v1/logout");
    struct curl_slist *headers;
    char auth[128];
    if(!curl) return;
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    headers = curl_slist_append(NULL, auth);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    perform(curl);
    curl_slist_free_all(headers); curl_easy_reset(curl);
    memset(auth, 0, sizeof(auth));
}

int service_resume(const char *token) {
    CURL *curl=request_new("/api/v1/me");
    char auth[128];
    if(!curl)return -1;
    snprintf(auth,sizeof(auth),"Authorization: Bearer %s",token);
    struct curl_slist *headers=curl_slist_append(NULL,auth);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    long result=perform(curl);
    curl_slist_free_all(headers);curl_easy_reset(curl);
    memset(auth,0,sizeof(auth));memset(response,0,sizeof(response));
    return result==200?0:result==401?-2:-1;
}

static struct curl_slist *authorize(CURL *curl,const char *token) {
    char auth[128];snprintf(auth,sizeof(auth),"Authorization: Bearer %s",token);
    struct curl_slist *headers=curl_slist_append(NULL,auth);
    memset(auth,0,sizeof(auth));curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    return headers;
}
static int list_filtered(const char *token,const char *owner,const char *game_filter,const char *filename,int page,remote_save_t *items,int *count,int *more) {
    *count=0;*more=0;
    if(owner && (strlen(owner)<3 || strlen(owner)>24 ||
       strspn(owner,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")!=strlen(owner))) {
        client_status("Enter a username: 3-24 letters, numbers or _.");return -1;
    }
    if(game_filter && strlen(game_filter)>80)return -1;
    char *escaped=curl_easy_escape(NULL,game_filter?game_filter:"",0);
    if(!escaped){client_status("Could not allocate search request.");return -1;}
    char path[400];snprintf(path,sizeof(path),"/api/v1/saves?scope=%s&page=%d%s%s&game=%s",
        owner?"public":"mine",page,owner?"&user=":"",owner?owner:"",escaped);
    curl_free(escaped);
    if(filename) {
        char *file=curl_easy_escape(NULL,filename,0);if(!file)return -1;
        size_t used=strlen(path);snprintf(path+used,sizeof(path)-used,"&filename=%s",file);curl_free(file);
    }
    CURL *curl=request_new(path);if(!curl)return -1;
    struct curl_slist *headers=authorize(curl,token);
    long status=perform(curl);curl_slist_free_all(headers);
    if(status!=200){curl_easy_reset(curl);return -1;}
    char *state,*line=strtok_r(response,"\n",&state);
    if(!line || (strcmp(line,"MORE\t0") && strcmp(line,"MORE\t1")))goto invalid;
    *more=line[5]=='1';
    while((line=strtok_r(NULL,"\n",&state))) {
        if(*count>=7)goto invalid;
        remote_save_t *out=&items[*count];memset(out,0,sizeof(*out));
        char *columns[9],*field_state;int n=0;
        for(char *c=strtok_r(line,"\t",&field_state);c;c=strtok_r(NULL,"\t",&field_state)) {
            if(n==9)goto invalid;
            columns[n++]=c;
        }
        if(n!=9)goto invalid;
        out->id=atoi(columns[0]);out->revision=atoi(columns[1]);out->size=atoi(columns[6]);out->header_offset=atoi(columns[8]);
        char *dest[]={out->filename,out->name,out->game,out->user,out->sha256};
        size_t caps[]={sizeof(out->filename),sizeof(out->name),sizeof(out->game),sizeof(out->user),sizeof(out->sha256)};
        int indices[]={2,3,4,5,7};
        for(int i=0;i<5;++i) {
            int length;char *decoded=curl_easy_unescape(curl,columns[indices[i]],0,&length);
            if(!decoded || length<1 || ((i==0||i==4) && (size_t)length>=caps[i])){curl_free(decoded);goto invalid;}
            if((size_t)length>=caps[i])length=caps[i]-1;
            memcpy(dest[i],decoded,length);dest[i][length]=0;curl_free(decoded);
            for(int j=0;j<length;++j)if((unsigned char)dest[i][j]<32 || (unsigned char)dest[i][j]>126)dest[i][j]='?';
        }
        if(out->id<=0||out->revision<=0||out->size<512||out->size>131072||out->size%512||
           out->header_offset<0||out->header_offset>=out->size/512||strlen(out->sha256)!=64||
           strspn(out->sha256,"0123456789abcdef")!=64||auth_is_save(out->filename,NULL,0))goto invalid;
        /* Fail closed if an older service ignores the owner filter. */
        if(owner && strcasecmp(owner,out->user)) {
            *count=0;*more=0;curl_easy_reset(curl);
            client_status("Service returned another user. Update service.");return -1;
        }
        if(filename && strcmp(filename,out->filename))goto invalid;
        ++*count;
    }
    curl_easy_reset(curl);return 0;
invalid:
    *count=0;*more=0;curl_easy_reset(curl);client_status("Invalid save list from service.");return -1;
}
typedef struct {unsigned char *bytes;size_t size,capacity;} download_buffer;
static size_t receive_download(char *data,size_t size,size_t count,void *context) {
    download_buffer *buffer=context;
    if(size && count>SIZE_MAX/size)return 0;
    size_t bytes=size*count;
    if(bytes>buffer->capacity-buffer->size)return 0;
    memcpy(buffer->bytes+buffer->size,data,bytes);buffer->size+=bytes;
    return bytes;
}
int service_icon(const char *token,const remote_save_t *item,unsigned char header[640]) {
    memset(header,0,640);
    if(item->size<512 || item->size>131072 || item->header_offset<0 ||
       item->header_offset>=item->size/512)return -1;
    int offset=item->header_offset*512;
    /* A short save cannot contain a complete icon. */
    if(item->size-offset<640)return 0;
    char path[96],range[32];
    snprintf(path,sizeof(path),"/api/v1/saves/%d/download?revision=%d",item->id,item->revision);
    snprintf(range,sizeof(range),"%d-%d",offset,offset+639);
    CURL *curl=request_new(path);if(!curl)return -1;
    download_buffer buffer={.bytes=header,.capacity=640};
    struct curl_slist *headers=authorize(curl,token);
    curl_easy_setopt(curl,CURLOPT_RANGE,range);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,using_modem?60000L:5000L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,using_modem?90000L:10000L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive_download);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);
    long status=perform(curl);
    curl_slist_free_all(headers);curl_easy_reset(curl);
    /* Reject ignored ranges and incomplete previews. The full save is still
       downloaded and SHA-256 checked separately before installation. */
    if(status!=206 || buffer.size!=640){memset(header,0,640);return -1;}
    return 0;
}
int service_list(const char *token,const char *owner,const char *game_filter,int page,remote_save_t *items,int *count,int *more) {
    return list_filtered(token,owner,game_filter,NULL,page,items,count,more);
}
int service_matches(const char *token,const char *filename,int page,remote_save_t *items,int *count,int *more) {
    return list_filtered(token,NULL,NULL,filename,page,items,count,more);
}
int service_rename(const char *token,const remote_save_t *item,const char *title) {
    char path[100];snprintf(path,sizeof(path),"/api/v1/saves/%d/rename",item->id);
    CURL *curl=request_new(path);if(!curl)return -1;
    char *escaped=curl_easy_escape(curl,title,0);if(!escaped){curl_easy_reset(curl);return -1;}
    char body[400];snprintf(body,sizeof(body),"name=%s&revision=%d",escaped,item->revision);curl_free(escaped);
    struct curl_slist *headers=authorize(curl,token);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body);
    long status=perform(curl);curl_slist_free_all(headers);curl_easy_reset(curl);
    if(status==200 && !strcmp(response,"OK\n"))return 0;
    client_status(status==409?"Title taken or save changed. Refresh and retry.":"Could not rename this save. Refresh and retry.");
    return -1;
}
int service_download(const char *token,const remote_save_t *item,void **data) {
    *data=NULL;
    if(item->size<512||item->size>131072||item->size%512)return -1;
    char path[96];snprintf(path,sizeof(path),"/api/v1/saves/%d/download?revision=%d",item->id,item->revision);
    CURL *curl=request_new(path);if(!curl)return -1;
    download_buffer buffer={.bytes=malloc(item->size),.capacity=item->size};
    if(!buffer.bytes)return -1;
    struct curl_slist *headers=authorize(curl,token);
    /* Drain the receive window promptly; curl throttling can stall KOS TCP. */
    curl_easy_setopt(curl,CURLOPT_MAX_RECV_SPEED_LARGE,(curl_off_t)0);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive_download);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);
    long status=perform(curl);curl_slist_free_all(headers);curl_easy_reset(curl);
    unsigned char digest[32];char hex[65];
    if(status!=200||buffer.size!=(size_t)item->size)goto fail;
    if(mbedtls_sha256(buffer.bytes,buffer.size,digest,0)!=0)goto fail;
    for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",digest[i]);
    if(strcmp(hex,item->sha256)||auth_is_save(item->filename,buffer.bytes,buffer.size))goto fail;
    *data=buffer.bytes;return 0;
fail:
    memset(buffer.bytes,0,buffer.size);free(buffer.bytes);
    client_status(status==409?"Save changed. Go back and refresh.":"Download failed or invalid. VMU unchanged.");return -1;
}
