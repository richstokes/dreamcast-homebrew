// HTTPS cartridge acquisition. All networking runs off the VM/render thread.
#include "download.h"
#include <kos.h>
#include <dc/flashrom.h>
#include <dc/modem/modem.h>
#include <ppp/ppp.h>
#include <mbedtls/ssl.h>
#include <curl/curl.h>
#include <mbedtls/platform_util.h>
#include <unistd.h>
#include <atomic>
#include <cctype>
#include <ctime>
#include <cstring>
#include <sstream>
#include <map>
#include <vector>
#include "cart.h"
#include "lodepng.h"

namespace {
mutex_t state_lock = MUTEX_INITIALIZER;
DownloadStatus state;
kthread_t* worker = nullptr;
std::atomic<bool> canceled{false};
std::string request_url;

void message(const std::string& text) {
    mutex_lock(&state_lock);
    state.message = text;
    mutex_unlock(&state_lock);
    printf("PICO8_NET: %s\n", text.c_str());
}
bool fail(const std::string& text) { message(text); return false; }
bool checkpoint() { return !canceled.load(); }
bool has_address(const uint8_t* ip) { return ip[0] || ip[1] || ip[2] || ip[3]; }

struct Modem {
    bool owned = false;
    ~Modem() { if (owned) ppp_shutdown(); }
    bool connect_link() {
        // BBA and modem occupy the same expansion socket: never probe the modem
        // when Ethernet exists, even if DHCP is still pending.
        if (net_default_dev) {
            message("Waiting for network address...");
            uint64_t end = timer_ms_gettime64() + 20000;
            while (checkpoint() && !has_address(net_default_dev->ip_addr) &&
                   timer_ms_gettime64() < end) thd_sleep(100);
            return checkpoint() && (has_address(net_default_dev->ip_addr) ||
                fail("No IP address. Check cable / DHCP."));
        }
        flashrom_ispcfg_t cfg{};
        bool saved = flashrom_get_pw_ispcfg(&cfg) == 0 &&
            (cfg.valid_fields & FLASHROM_ISP_PHONE1) && cfg.phone1[0];
        if (!saved) saved = flashrom_get_ispcfg(&cfg) == 0 &&
            (cfg.valid_fields & FLASHROM_ISP_PHONE1) && cfg.phone1[0];
        if (!saved) { memset(&cfg, 0, sizeof(cfg)); strcpy(cfg.phone1, "555"); }
        // Honor a saved profile as a whole; don't silently dial a changed number.
        if (cfg.flags & FLASHROM_ISP_PULSE_DIAL) {
            mbedtls_platform_zeroize(&cfg, sizeof(cfg));
            return fail("Set a tone-dial ISP profile first.");
        }
        std::string number;
        if (((cfg.valid_fields & FLASHROM_ISP_OUT_PREFIX) && cfg.out_prefix[0]) ||
            ((cfg.valid_fields & FLASHROM_ISP_CW_PREFIX) && cfg.cw_prefix[0]) ||
            ((cfg.valid_fields & FLASHROM_ISP_LD_PREFIX) && cfg.ld_prefix[0])) {
            mbedtls_platform_zeroize(&cfg, sizeof(cfg));
            return fail("Dial prefixes need browser setup.");
        }
        if (cfg.flags & FLASHROM_ISP_DIAL_AREACODE)
            number.append(cfg.p1_areacode, strnlen(cfg.p1_areacode, sizeof(cfg.p1_areacode)));
        number.append(cfg.phone1, strnlen(cfg.phone1, sizeof(cfg.phone1)));
        std::string digits;
        for (char c : number) {
            if (strchr("0123456789*#ABCD", c)) digits += c;
            else if (!strchr(" -().", c)) {
                mbedtls_platform_zeroize(&cfg, sizeof(cfg));
                return fail("Unsupported dial number in profile.");
            }
        }
        if (digits.empty() || digits.size() > 63) {
            mbedtls_platform_zeroize(&cfg, sizeof(cfg));
            return fail("Invalid dial number in ISP profile.");
        }
        message(saved ? "Dialing saved ISP profile..." : "Dialing DreamPi (555)...");
        if (ppp_init() < 0) {
            mbedtls_platform_zeroize(&cfg, sizeof(cfg));
            return fail("PPP initialization failed.");
        }
        owned = true;
        char login[sizeof(cfg.ppp_login) + 1]{}, password[sizeof(cfg.ppp_passwd) + 1]{};
        snprintf(login, sizeof(login), "%.*s", int(sizeof(cfg.ppp_login)),
                 cfg.valid_fields & FLASHROM_ISP_PPP_USER ? cfg.ppp_login : "dream");
        snprintf(password, sizeof(password), "%.*s", int(sizeof(cfg.ppp_passwd)),
                 cfg.valid_fields & FLASHROM_ISP_PPP_PASS ? cfg.ppp_passwd : "cast");
        int result = ppp_set_login(login, password);
        int blind = !!(cfg.flags & FLASHROM_ISP_BLIND_DIAL);
        uint8_t dns[4]{};
        if (cfg.valid_fields & FLASHROM_ISP_DNS) {
            memcpy(dns, has_address(cfg.dns[0]) ? cfg.dns[0] : cfg.dns[1], 4);
        }
        mbedtls_platform_zeroize(login, sizeof(login));
        mbedtls_platform_zeroize(password, sizeof(password));
        mbedtls_platform_zeroize(&cfg, sizeof(cfg));
        if (result < 0) return fail("PPP login setup failed.");
        if (!checkpoint()) return false;
        result = ppp_modem_init(digits.c_str(), blind, nullptr);
        if (result < 0) return fail(result == -1 ? "No BBA or modem detected." :
            result == -2 ? "No dial tone. Check modem setup." : "Dial failed. Check modem / ISP.");
        if (!checkpoint()) return false;
        message("Negotiating PPP connection...");
        if (ppp_connect() < 0) return fail("PPP failed. Check ISP login.");
        if (!checkpoint()) return false;
        if (!net_default_dev || !has_address(net_default_dev->ip_addr))
            return fail("PPP did not assign an IP address.");
        if (!has_address(net_default_dev->dns)) memcpy(net_default_dev->dns, dns, 4);
        return true;
    }
};

struct Url { std::string host, authority, path; unsigned port = 443; };
bool parse_url(const std::string& input, Url& out) {
    if (input.size() > DC_URL_LIMIT || input.compare(0, 8, "https://") != 0)
        return fail("Enter an https:// cartridge URL.");
    for (unsigned char c : input)
        if (c <= 32 || c >= 127 || c == '\\') return fail("URL needs ASCII / percent encoding.");
    size_t end = input.find_first_of("/?#", 8);
    out.authority = input.substr(8, end == std::string::npos ? end : end - 8);
    auto colon = out.authority.find(':');
    out.host = out.authority.substr(0, colon);
    if (out.host.empty() || out.host.size() > 253)
        return fail("Invalid server name.");
    for (unsigned char c : out.host)
        if (!std::isalnum(c) && c != '-' && c != '.') return fail("Use a DNS name or IPv4 address.");
    out.port = 443;
    if (colon != std::string::npos) {
        auto port = out.authority.substr(colon + 1);
        if (port.empty() || port.size() > 5) return fail("Invalid HTTPS port.");
        unsigned n = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return fail("Invalid HTTPS port.");
            n = n * 10 + c - '0';
        }
        if (!n || n > 65535) return fail("Invalid HTTPS port.");
        out.port = n;
    }
    out.path = end == std::string::npos ? "/" : input.substr(end);
    auto fragment = out.path.find('#');
    if (fragment != std::string::npos) out.path.resize(fragment);
    if (out.path.empty() || out.path[0] == '?') out.path.insert(0, "/");
    return true;
}

// kos-ports disables MBEDTLS_HAVE_TIME_DATE. Check EVERY certificate's dates
// explicitly without changing the installed SDK or weakening chain/name checks.
int verify_date(void*, mbedtls_x509_crt* cert, int, uint32_t* flags) {
    time_t now = time(nullptr);
    tm utc{};
    if (!gmtime_r(&now, &utc) || utc.tm_year < 125 || utc.tm_year > 200) {
        *flags |= MBEDTLS_X509_BADCERT_OTHER;
        return 0;
    }
    auto stamp = [](int y, int m, int d, int h, int min, int s) -> uint64_t {
        return (((((uint64_t(y) * 13 + m) * 32 + d) * 24 + h) * 60 + min) * 60 + s);
    };
    auto cvt = [&](const mbedtls_x509_time& t) {
        return stamp(t.year, t.mon, t.day, t.hour, t.min, t.sec);
    };
    auto current = stamp(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                         utc.tm_hour, utc.tm_min, utc.tm_sec);
    if (current < cvt(cert->valid_from)) *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    if (current > cvt(cert->valid_to)) *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    return 0;
}

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
struct Transfer {
    std::vector<unsigned char> bytes;
    size_t header_bytes = 0;
    std::string error;
};
CURLcode configure_tls(CURL*, void* context, void*) {
    // The installed curl uses Mbed TLS; retain mandatory chain/name checks and
    // supply the date checks omitted by this kos-port's TLS configuration.
    auto* config = static_cast<mbedtls_ssl_config*>(context);
    mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_verify(config, verify_date, nullptr);
    return CURLE_OK;
}
size_t receive_bytes(char* data, size_t size, size_t count, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    if (!checkpoint()) return 0;
    if (size && count > (DC_CART_LIMIT - transfer.bytes.size()) / size) {
        transfer.error = "Cart exceeds the 1 MiB limit."; return 0;
    }
    size_t bytes = size * count;
    transfer.bytes.insert(transfer.bytes.end(), data, data + bytes);
    return bytes;
}
size_t receive_header(char* data, size_t size, size_t count, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    size_t bytes = size * count;
    if (!checkpoint()) return 0;
    std::string line(data, bytes);
    if (line.compare(0, 5, "HTTP/") == 0) {
        transfer.header_bytes = 0;
        transfer.bytes.clear();
    }
    transfer.header_bytes += bytes;
    if (transfer.header_bytes > 16384) {
        transfer.error = "HTTP headers exceed limit."; return 0;
    }
    auto colon = line.find(':');
    auto key = line.substr(0, colon);
    for (auto& c : key) c = std::tolower(static_cast<unsigned char>(c));
    if (key == "location" && bytes > DC_URL_LIMIT + 16) {
        transfer.error = "Redirect URL exceeds limit."; return 0;
    }
    if (key == "content-encoding" && trim(line.substr(colon + 1)) != "identity") {
        transfer.error = "Unsupported HTTP content encoding."; return 0;
    }
    if (line == "\r\n") message("Downloading cartridge...");
    return bytes;
}
int progress(void*, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t) {
    if (!checkpoint()) return 1;
    mutex_lock(&state_lock);
    state.received = received > 0 ? size_t(received) : 0;
    state.total = total > 0 ? size_t(total) : 0;
    mutex_unlock(&state_lock);
    return 0;
}
bool fetch(const std::string& url, std::vector<unsigned char>& bytes) {
    // Use the same maintained HTTP/TLS stack as this workspace's DCVMU client.
    // Initialize and clean up on the sole network worker, before PPP teardown.
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return fail("HTTPS initialization failed.");
    const auto* version = curl_version_info(CURLVERSION_NOW);
    if (!version->ssl_version || strncmp(version->ssl_version, "mbedTLS/", 8)) {
        curl_global_cleanup(); return fail("Build curl with the Mbed TLS backend.");
    }
    CURL* curl = curl_easy_init();
    if (!curl) { curl_global_cleanup(); return fail("Cannot allocate HTTPS client."); }
    Transfer transfer;
    message("Connecting and verifying HTTPS...");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/rd/certs/ca-bundle.pem");
    curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, configure_tls);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 4096L);
    curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, curl_off_t(65536));
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, curl_off_t(DC_CART_LIMIT));
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dreamcast-PICO8/1");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_bytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    CURLcode result = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    if (!checkpoint()) return false;
    if (result != CURLE_OK || http != 200) {
        printf("PICO8_NET: curl=%d http=%ld\n", int(result), http);
        if (!transfer.error.empty()) return fail(transfer.error);
        if (result == CURLE_PEER_FAILED_VERIFICATION)
            return fail("HTTPS failed. Check URL / console date.");
        if (result == CURLE_FILESIZE_EXCEEDED) return fail("Cart exceeds the 1 MiB limit.");
        if (result == CURLE_OPERATION_TIMEDOUT) return fail("Network timeout. Please retry.");
        if (result == CURLE_TOO_MANY_REDIRECTS) return fail("Too many HTTPS redirects.");
        if (result == CURLE_UNSUPPORTED_PROTOCOL) return fail("Redirect must use HTTPS.");
        if (result == CURLE_COULDNT_RESOLVE_HOST) return fail("DNS lookup failed. Check connection.");
        if (http != 200 && http) {
            char error[48]; snprintf(error, sizeof(error), "Server returned HTTP %ld.", http);
            return fail(error);
        }
        return fail("Download failed or was cut short.");
    }
    bytes = std::move(transfer.bytes);
    return true;
}

bool save_cart(const std::vector<unsigned char>& bytes, std::string& path) {
    message("Checking cartridge...");
    bool png = bytes.size() >= 24 && memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    if (png) {
        // Reject oversized images before allowing the decoder to allocate pixels.
        unsigned w = 0, h = 0;
        lodepng::State info;
        if (lodepng_inspect(&w, &h, &info, bytes.data(), bytes.size()) || w != 160 || h != 205)
            return fail("PNG must be a 160 x 205 PICO-8 cart.");
    } else {
        std::string text(bytes.begin(), bytes.end());
        if (text.compare(0, 16, "pico-8 cartridge") != 0 || text.find("\n__lua__") == std::string::npos)
            return fail("Use a direct .p8 / .p8.png link.");
        if (text.find("#include") != std::string::npos)
            return fail("Export a single-file cart first.");
        if (text.find('\0') != std::string::npos) return fail("Invalid text cartridge.");
        // Upstream text conversion helpers assume well-sized hex sections.
        // Check these before they can write into the fixed cartridge arrays.
        std::istringstream lines(text);
        std::string line, section;
        std::map<std::string, size_t> counts;
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.compare(0, 2, "__") == 0) { section = line; continue; }
            if (line.empty() || section == "__lua__") continue;
            size_t limit = section == "__gfx__" || section == "__label__" ? 16384 :
                section == "__gff__" ? 512 : section == "__map__" ? 8192 : 0;
            if (limit) {
                if (line.size() % 2 || (counts[section] += line.size()) > limit)
                    return fail("Invalid cartridge data section.");
                for (unsigned char c : line)
                    if (!std::isxdigit(c)) return fail("Invalid cartridge hex data.");
            } else if (section == "__sfx__" || section == "__music__") {
                if (++counts[section] > 64 || line.size() != (section == "__sfx__" ? 168u : 11u))
                    return fail("Invalid cartridge audio section.");
                for (size_t i = 0; i < line.size(); ++i)
                    if (!(section == "__music__" && i == 2 && line[i] == ' ') &&
                        !std::isxdigit(static_cast<unsigned char>(line[i])))
                        return fail("Invalid cartridge audio data.");
            }
        }
    }
    Cart check(bytes.data(), bytes.size());
    if (!check.LoadError.empty() || check.LuaString.empty())
        return fail("Invalid or empty PICO-8 cartridge.");
    if (!checkpoint()) return false;
    path = png ? "/ram/download.p8.png" : "/ram/download.p8";
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return fail("Not enough RAM to save download.");
    bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (fclose(file) != 0) ok = false;
    if (!ok) { unlink(path.c_str()); return fail("Could not store complete cartridge."); }
    return true;
}

void* run(void*) {
    bool ok = false;
    std::string path, url = request_url;
    {
        Modem modem;
        Url parsed;
        if (parse_url(url, parsed) && checkpoint() && modem.connect_link()) {
            std::vector<unsigned char> body;
            ok = fetch(url, body) && save_cart(body, path);
        }
    } // Hang up only the PPP connection this request owns, before launch.
    if (!checkpoint()) { if (ok) unlink(path.c_str()); ok = false; message("Download canceled."); }
    if (ok) message("Cartridge ready.");
    mutex_lock(&state_lock);
    state.success = ok; state.path = ok ? path : ""; state.busy = false;
    mutex_unlock(&state_lock);
    return nullptr;
}
}

DownloadStatus dc_download_status() {
    mutex_lock(&state_lock); auto result = state; mutex_unlock(&state_lock);
    return result;
}
bool dc_download_start(const std::string& url) {
    if (dc_download_status().busy) return false;
    dc_download_reap();
    request_url = url; canceled = false;
    mutex_lock(&state_lock);
    state = {}; state.busy = true; state.message = "Starting download...";
    mutex_unlock(&state_lock);
    kthread_attr_t attr{};
    attr.stack_size = 256 * 1024; attr.prio = PRIO_DEFAULT + 1; attr.label = "PICO-8 HTTPS";
    worker = thd_create_ex(&attr, run, nullptr);
    if (worker) return true;
    mutex_lock(&state_lock); state.busy = false; state.message = "Cannot start network worker.";
    mutex_unlock(&state_lock);
    return false;
}
void dc_download_cancel() { canceled = true; }
void dc_download_reap() {
    if (worker && !dc_download_status().busy) { thd_join(worker, nullptr); worker = nullptr; }
}
