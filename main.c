#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>

#include <curl/curl.h>

typedef struct {
    bool debug;

    bool verbose;
    int headerc;
    char *headers[128];
    bool head;
    char *method;
    char *data;
    char *data_ascii;
    char *data_binary;
    char *data_raw;
    char *data_urlencode;
    bool get;
    int formc;
    struct {
        bool is_file;
        char *name;
        char *value;
    } forms[128];
    char *cookie;
    char *cookie_jar;
    bool append;
    char *upload_file;

    int urlc;
    char **urls;

    int requests;
    int timelimit;
    int concurrency;
} config_t;

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return size * nmemb;
}

CURL *make_curl(const config_t *cfg, int *idx) {
    int i;
    CURL *curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_PRIVATE, idx);

    // set URL
    curl_easy_setopt(curl, CURLOPT_URL, cfg->urls[(*idx) ++]);
    if(*idx >= cfg->urlc) *idx = 0;

    if(cfg->verbose) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
        curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    }

    // set HEADER

    return curl;
}

enum {
    DATA_ASCII = 128,
    DATA_BINARY,
    DATA_RAW,
    DATA_URLENCODE,

    FORM_STRING,
};
static const char *options = "hVDvH:Im:d:GF:C:J:aT:n:t:c:";
static struct option OPTIONS[] = {
    {"help",            0, 0, 'h' },
    {"version",         0, 0, 'V' },
    {"debug",           0, 0, 'D' },

    {"verbose",         0, 0, 'v' },
    {"header",          1, 0, 'H' },
    {"head",            0, 0, 'I'},
    {"method",          1, 0, 'm' },
    {"data",            1, 0, 'd' },
    {"data-ascii",      1, 0, DATA_ASCII },
    {"data-binary",     1, 0, DATA_BINARY },
    {"data-raw",        1, 0, DATA_RAW },
    {"data-urlencode",  1, 0, DATA_URLENCODE },
    {"get",             0, 0, 'G' },
    {"form",            1, 0, 'F' },
    {"form-string",     1, 0, FORM_STRING },
    {"cookie",          1, 0, 'C' },
    {"cookie-jar",      1, 0, 'J' },
    {"append",          0, 0, 'a' },
    {"upload-file",     1, 0, 'T' },

    {"requests",        0, 0, 'n' },
    {"timelimit",       0, 0, 't' },
    {"concurrency",     0, 0, 'c' },

    {NULL,              0, 0, 0 }
};

void usage(char *argv0) {
    printf(
        "Usage: %s [options...] <url>...\n"
        "  -h,--help                         This help\n"
        "  -V,--version                      Show curl version\n"
        "  -D,--debug                        Show debug info\n"

        "  -v,--verbose                      Make the operation more talkative\n"
        "  -H,--header <header>              Set custom request header\n"
        "  -I,--head                         Show document info only\n"
        "  -m,--method <method>              Custom request method\n"
        "  -d,--data <data>                  HTTP POST data\n"
        "     --data-ascii <data>            HTTP POST ASCII data\n"
        "     --data-binary <data>           HTTP POST binary data\n"
        "     --data-raw <data>              HTTP POST data, '@' allowed\n"
        "     --data-urlencode <data>        HTTP POST url encoded\n"
        "  -G,--get                          Put the post data in the URL and use GET\n"
        "  -F,--form <name=content>          Specify multipart MIME data\n"
        "     --form-string <name=string>    Specify multipart MIME data\n"
        "  -C,--cookie <data|filename>       Send cookies from string/file\n"
        "  -J,--cookie-jar <filename>        Write cookies to <filename> after operation\n"
        "  -a,--append                       Append to target file when uploading\n"
        "  -T,--upload-file <file>           Transfer local FILE to destination\n"

        "  -n,--requests <requests>          Number of requests to perform\n"
        "  -t,--timelimit <seconds>          Seconds to max. to spend on benchmarking\n"
        "  -c,--concurrency <concurrency>    Number of multiple requests to make at a time\n"
        , argv0
    );
}

volatile bool is_running = true, is_timer = false;
void sig_handler(int sig) {
    if(sig == SIGALRM) {
        is_timer = true;
    } else {
        is_running = false;
        // printf("SIG: %d\n", sig);
    }
}

int main(int argc, char *argv[]) {
    config_t cfg;
    int c, idx = 0, *idxs;
    CURLM *multi;

    memset(&cfg, 0, sizeof(cfg));

    cfg.concurrency = 10;

    while((c = getopt_long(argc, argv, options, OPTIONS, &idx)) != -1) {
        switch(c) {
            case 'V':
                printf("curl %s\n", LIBCURL_VERSION);
                exit(0);
                break;
            case 'D':
                cfg.debug = true;
                break;

            case 'v':
                cfg.verbose = true;
                break;
            case 'H': // header
                if(cfg.headerc >= sizeof(cfg.headers)/sizeof(cfg.headers[0])) {
                    printf("form argument too many: %s\n", optarg);
                } else {
                    cfg.headers[cfg.headerc++] = optarg;
                }
                break;
            case 'I': // head
                cfg.head = true;
                break;
            case 'm': // method
                cfg.method = optarg;
                break;
            case 'd': // data
                cfg.data = optarg;
                break;
            case DATA_ASCII: // data-ascii
                cfg.data_ascii = optarg;
                break;
            case DATA_BINARY: // data-binary
                cfg.data_binary = optarg;
                break;
            case DATA_RAW: // data-raw
                cfg.data_raw = optarg;
                break;
            case DATA_URLENCODE: // data-urlencode
                cfg.data_urlencode = optarg;
                break;
            case 'G': // get
                cfg.get = true;
                break;
            case FORM_STRING: // form-string
            case 'F': { // form
                if(cfg.formc >= sizeof(cfg.forms)/sizeof(cfg.forms[0])) {
                    printf("form argument too many: %s\n", optarg);
                    break;
                }
                char *p = strchr(optarg, '=');
                if(p) {
                    *p++ = '\0';
                    bool is_file = (c == 'F' && *p == '@');
                    cfg.forms[cfg.formc].is_file = is_file;
                    cfg.forms[cfg.formc].name = strndup(optarg, p - optarg);
                    cfg.forms[cfg.formc].value = strdup(is_file ? p + 1 : p);
                } else {
                    cfg.forms[cfg.formc].name = strdup(optarg);
                    cfg.forms[cfg.formc].value = strdup("");
                }
                cfg.formc++;
                break;
            }
            case 'C': // cookie
                cfg.cookie = optarg;
                break;
            case 'J': // cookie-jar
                cfg.cookie_jar = optarg;
                break;
            case 'a':
                cfg.append = true;
                break;
            case 'T':
                cfg.upload_file = optarg;
                break;
            
            case 'n': // requests
                cfg.requests = atoi(optarg);
                break;
            case 't': // timelimit
                cfg.timelimit = atoi(optarg);
                break;
            case 'c': // concurrency
                cfg.concurrency = atoi(optarg);
                if(cfg.concurrency <= 0) cfg.concurrency = 1;
                break;

            case 'h':
            default:
                usage(argv[0]);
                exit(EXIT_FAILURE);
                break;
        }
    }
    if(optind >= argc) {
        fprintf(stderr, "ERROR: At least one URL.\n");
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    cfg.urlc = argc - optind;
    cfg.urls = argv + optind;

    if(cfg.debug) {
        printf("======== CONFIG INFO BEGIN ========\n");
        printf("verbose: %s\n", cfg.verbose ? "true" : "false");
        printf("headers: %d\n", cfg.headerc);
        for(c=0; c<cfg.headerc; c++) {
            printf("  %d => %s\n", c, cfg.headers[c]);
        }
        printf("head: %s\n", cfg.head ? "true" : "false");
        printf("method: %s\n", cfg.method);
        printf("data: %s\n", cfg.data);
        printf("data_ascii: %s\n", cfg.data_ascii);
        printf("data_binary: %s\n", cfg.data_binary);
        printf("data_raw: %s\n", cfg.data_raw);
        printf("data_urlencode: %s\n", cfg.data_urlencode);
        printf("get: %s\n", cfg.get ? "true" : "false");
        printf("forms: %d\n", cfg.formc);
        for(c=0; c<cfg.formc; c++) {
            printf("  %d => is_file: %s, name: %s, value: %s\n", c, cfg.forms[c].is_file ? "true" : "false", cfg.forms[c].name, cfg.forms[c].value);
        }
        printf("cookie: %s\n", cfg.cookie);
        printf("cookie_jar: %s\n", cfg.cookie_jar);
        printf("append: %s\n", cfg.append ? "true" : "false");
        printf("upload_file: %s\n", cfg.upload_file);
        printf("\n");
        printf("urls: %d\n", cfg.urlc);
        for(c=0; c<cfg.urlc; c++) {
            printf("  %d => %s\n", c, cfg.urls[c]);
        }
        printf("\n");
        printf("requests: %d\n", cfg.requests);
        printf("timelimit: %d\n", cfg.timelimit);
        printf("concurrency: %d\n", cfg.concurrency);
        printf("========= CONFIG INFO END =========\n");
    }

    curl_global_init(CURL_GLOBAL_ALL);

    idxs = (int*) malloc(sizeof(int)*cfg.concurrency);
    multi = curl_multi_init();

    for(c=0; c<cfg.concurrency; c++) {
        idxs[c] = 0;
        curl_multi_add_handle(multi, make_curl(&cfg, &idxs[c]));
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGQUIT, sig_handler);
    signal(SIGUSR1, sig_handler);
    signal(SIGUSR2, sig_handler);

    {
        struct itimerval itv;

        itv.it_interval.tv_sec = itv.it_value.tv_sec = 1;
        itv.it_interval.tv_usec = itv.it_value.tv_usec = 0;

        setitimer(ITIMER_REAL, &itv, NULL);
        signal(SIGALRM, sig_handler);
    }

    {
        time_t timelimit = time(NULL) + cfg.timelimit;
        int still_running, msgs, requests = 0, prevs = 0;
        CURL *curl;
        CURLMcode mc;
        struct CURLMsg *m;
        int *idx, code;
        int code1xx = 0, code2xx = 0, code3xx = 0, code4xx = 0, code5xx = 0, codex = 0;
        int concurrency = cfg.concurrency;
        int times = 0;

        do {
            still_running = 0;
            mc = curl_multi_perform(multi, &still_running);

            if(mc) {
                printf("curl_multi_perform error: %s\n", curl_multi_strerror(mc));
                break;
            }

            do {
                msgs = 0;
                m = curl_multi_info_read(multi, &msgs);
                if(m && (m->msg && CURLMSG_DONE)) {
                    requests ++;
                    curl = m->easy_handle;

                    code = 0;
                    idx = NULL;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
                    curl_easy_getinfo(curl, CURLINFO_PRIVATE, &idx);

                    curl_multi_remove_handle(multi, curl);
                    curl_easy_cleanup(curl);
                    curl = NULL;

                    if(code < 200) {
                        code1xx ++;
                    } else if(code < 300) {
                        code2xx ++;
                    } else if(code < 400) {
                        code3xx ++;
                    } else if(code < 500) {
                        code4xx ++;
                    } else if(code < 600) {
                        code5xx ++;
                    } else {
                        codex ++;
                    }

                    if(is_running && (cfg.requests <= 0 || requests < cfg.requests) && (cfg.timelimit <= 0 || timelimit >= time(NULL))) {
                        curl_multi_add_handle(multi, make_curl(&cfg, idx));
                    } else {
                        concurrency --;
                    }
                }
            } while(msgs);

            if(still_running) {
                mc = curl_multi_poll(multi, NULL, 0, 1000, NULL);
                if(mc) {
                    printf("curl_multi_poll error: %s\n", curl_multi_strerror(mc));
                    break;
                }
            }

            if(is_timer || !concurrency) {
                is_timer = false;
                if(!is_running && isatty(2)) printf("\033[2K\r");
                printf("times: %d, concurrency: %d, 1xx: %d, 2xx: %d, 3xx: %d, 4xx: %d, 5xx: %d, xxx: %d, reqs: %d/s\n", ++times, concurrency, code1xx, code2xx, code3xx, code4xx, code5xx, codex, requests - prevs);
                prevs = requests;
            }
        } while(concurrency);
    }

    curl_multi_cleanup(multi);
    curl_global_cleanup();

    for(c=0; c<cfg.formc; c++) {
        free(cfg.forms[c].name);
        free(cfg.forms[c].value);
    }

    return 0;
}