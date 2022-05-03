#define _GNU_SOURCE // asprintf declare
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>

#include <curl/curl.h>

typedef struct {
    bool info;
    char *debug;

    bool verbose;
    int headerc;
    char *headers[128];
    bool head;
    char *method;
    char *data;
    bool get;
    int formc;
    struct {
        bool is_file;
        char *name;
        char *value;
    } forms[128];
    char *cookie;
    char *cookie_file;
    bool cookie_session;
    bool append;
    char *upload_file;

    bool keepalive;

    int urlc, *urlw;
    char **urls;

    int requests;
    int timelimit;
    int concurrency;
} config_t;

typedef struct {
    int i, w;
    int reqs;
    char *logfile;
    FILE *logfp;
    double time;
} idx_t;

typedef struct {
    idx_t *idx;
    struct curl_slist *headers;
    struct curl_httppost *form;
    FILE *fp_upload;
} request_t;

char *nowtime(void) {
	static char buf[64];
	struct timeval tv = {0, 0};
	struct tm tm;

	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);

	snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
		tm.tm_year + 1900, tm.tm_mon, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		tv.tv_usec
	);

	return buf;
}

char *fsize(long int size, char *buf) {
    const char *units = "0KMGTPEZY";
    int unit;

    if(size <= 0) {
        return "0.000";
    }
    
    unit = (int)(log(size)/log(1024));
    if(unit > 8) {
        unit = 8;
    }

    sprintf(buf, "%.2lf%c", size / pow(1024,unit), units[unit]);

    return buf;
}

double microtime() {
    struct timeval tv = {0, 0};

    if(gettimeofday(&tv, NULL)) tv.tv_sec = time(NULL);

    return (double) tv.tv_sec + tv.tv_usec / 1000000.0f;
}

static long int req_bytes = 0, res_bytes = 0, bug_bytes = 0;
int debug_bytes_handler(CURL *handle, curl_infotype type, char *data, size_t size, void *userp) {
    switch (type) {
		case CURLINFO_HEADER_OUT:
		case CURLINFO_DATA_OUT:
			res_bytes += size;
			break;
		case CURLINFO_HEADER_IN:
		case CURLINFO_DATA_IN:
			req_bytes += size;
			break;
		case CURLINFO_TEXT:
			bug_bytes += size;
			break;
	}

    return 0;
}

int debug_handler(CURL *handle, curl_infotype type, char *data, size_t size, void *userp) {
    idx_t *idx = (idx_t*) userp;
    int ret = 0;

    debug_bytes_handler(handle, type, data, size, userp);

    if(!idx->logfp) return 0;

	switch (type) {
		case CURLINFO_HEADER_OUT:
			do {
				char *ptr;
				while(size) {
					ptr = data;
					
					if(*ptr) {
						while(*ptr != '\r' && *ptr != '\n') {
							ptr ++;
							size --;
						}
						
						if(*ptr == '\r' && *(ptr+1) == '\n') {
							ptr +=2 ;
							size -=2;
						}
					}
					
					fprintf(idx->logfp, "%s > ", nowtime());
                    fwrite(data, 1, ptr-data, idx->logfp);
					data = ptr;
				}
			} while(0);
			break;
		case CURLINFO_DATA_OUT:
            fwrite(data, 1, size, idx->logfp);
			break;
		case CURLINFO_HEADER_IN:
            fprintf(idx->logfp, "%s < ", nowtime());
            fwrite(data, 1, size, idx->logfp);
			break;
		case CURLINFO_DATA_IN:
            fwrite(data, 1, size, idx->logfp);
			break;
		case CURLINFO_TEXT:
            fprintf(idx->logfp, "%s * ", nowtime());
            fwrite(data, 1, size, idx->logfp);
			break;
	}

	return 0;
}

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return size * nmemb;
}

size_t read_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return fread(ptr, size, nmemb, (FILE*) userdata);
}

CURL *make_curl(const config_t *cfg, idx_t *idx) {
    int i;
    CURL *curl = curl_easy_init();
    request_t *req = (request_t*) malloc(sizeof(request_t));

    memset(req, 0, sizeof(request_t));
    req->idx = idx;

    curl_easy_setopt(curl, CURLOPT_PRIVATE, req);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

    if(idx->logfile) { // set DEBUG
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA, idx);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_handler);

        if(!idx->logfp || access(idx->logfile, F_OK)) {
            if(idx->logfp) fclose(idx->logfp);
            idx->logfp = fopen(idx->logfile, "w");
            if(!idx->logfp) fprintf(stderr, "open %s failure: %s\n", idx->logfile, strerror(errno));
        }
    } else if(cfg->verbose) { // set VERBOSE
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA, idx);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_handler);
    } else {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_bytes_handler);
    }

    if(idx->logfp) fprintf(idx->logfp, "%s * BEGIN %dst REQUEST\n", nowtime(), ++ idx->reqs);

    // weight
    if(cfg->urlw) {
        if(idx->w < cfg->urlw[idx->i]) {
            i = idx->i;
            if(++idx->w >= cfg->urlw[idx->i]) {
                idx->i ++;
                idx->w = 0;
            }
        } else {
            idx->w = 0;
            i = idx->i ++;
        }
    } else {
        i = idx->i ++;
    }
    if(idx->i >= cfg->urlc) {
        idx->i = 0;
    }
    // printf("  %d => [%d] %s\n", i, cfg->urlw ? cfg->urlw[i] : 1, cfg->urls[i]);

    // set URL
    curl_easy_setopt(curl, CURLOPT_URL, cfg->urls[i]);

    // set HEADER
    if(cfg->headerc) {
        for(i=0; i<cfg->headerc; i++) {
            req->headers = curl_slist_append(req->headers, cfg->headers[i]);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req->headers);
    }

    // set DATA
    if(cfg->data) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, cfg->data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(cfg->data));
    }

    // set HEAD
    if(cfg->head) curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    // set FORM
    if(cfg->formc) {
        struct curl_httppost *lastptr = NULL;
        for(i=0; i<cfg->formc; i++) {
            if(cfg->forms[i].is_file) {
                if(access(cfg->forms[i].value, R_OK)) {
                    fprintf(stderr, "form file not exists: name: %s, value: %s\n", cfg->forms[i].name, cfg->forms[i].value);
                } else {
                    curl_formadd(&req->form, &lastptr,
                        CURLFORM_PTRNAME, cfg->forms[i].name,
                        CURLFORM_FILE, cfg->forms[i].value,
                        CURLFORM_END
                    );
                }
            } else {
                curl_formadd(&req->form, &lastptr,
                    CURLFORM_PTRNAME, cfg->forms[i].name,
                    CURLFORM_PTRCONTENTS, cfg->forms[i].value,
                    CURLFORM_END
                );
            }
        }
        curl_easy_setopt(curl, CURLOPT_HTTPPOST, req->form);
    }

    // set GET
    if(cfg->get) curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    // set COOKIE
    if(cfg->cookie) curl_easy_setopt(curl, CURLOPT_COOKIE, cfg->cookie);

    // set COOKIE_FILE
    if(cfg->cookie_file) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cfg->cookie_file);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cfg->cookie_file);
    }

    // set COOKIE_SESSION
    if(cfg->cookie_session) curl_easy_setopt(curl, CURLOPT_COOKIESESSION, cfg->cookie_session);

    if(cfg->append) curl_easy_setopt(curl, CURLOPT_APPEND, 1L);
    if(cfg->upload_file) {
        req->fp_upload = fopen(cfg->upload_file, "r");
        if(req->fp_upload) {
            fseek(req->fp_upload, 0, SEEK_END);
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl, CURLOPT_READDATA, (void*) req->fp_upload);
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
            curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) ftell(req->fp_upload));
            fseek(req->fp_upload, 0, SEEK_SET);
        } else {
            fprintf(stderr, "open %s failure: %s\n", cfg->upload_file, strerror(errno));
        }
    }

    // set KEEPALIVE
    if(cfg->keepalive) {
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0);
    }

    // set METHOD
    if(cfg->method) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cfg->method);

    idx->time = microtime();

    return curl;
}

enum {
    FORM_STRING = 128,
};
static const char *options = "hViD:vH:Im:d:GF:C:f:saT:kn:t:c:w:";
static struct option OPTIONS[] = {
    {"help",            0, 0, 'h' },
    {"version",         0, 0, 'V' },
    {"info",            0, 0, 'i' },
    {"debug",           1, 0, 'D' },

    {"verbose",         0, 0, 'v' },
    {"header",          1, 0, 'H' },
    {"head",            0, 0, 'I'},
    {"method",          1, 0, 'm' },
    {"data",            1, 0, 'd' },
    {"get",             0, 0, 'G' },
    {"form",            1, 0, 'F' },
    {"form-string",     1, 0, FORM_STRING },
    {"cookie",          1, 0, 'C' },
    {"cookie-file",     1, 0, 'f' },
    {"cookie-session",  0, 0, 's' },
    {"append",          0, 0, 'a' },
    {"upload-file",     1, 0, 'T' },
    {"keepalive",       1, 0, 'k' },

    {"requests",        0, 0, 'n' },
    {"timelimit",       0, 0, 't' },
    {"concurrency",     0, 0, 'c' },

    {"weight",     0, 0, 'w' },

    {NULL,              0, 0, 0 }
};

void usage(char *argv0) {
    printf(
        "Usage: %s [options...] <url>...\n"
        "  -h,--help                         This help\n"
        "  -V,--version                      Show curl version\n"
        "  -i,--info                         Show config info\n"
        "  -D,--debug <path>                 Save debug info to <path>\n"

        "  -v,--verbose                      Make the operation more talkative\n"
        "  -H,--header <header>              Set custom request header\n"
        "  -I,--head                         Show document info only\n"
        "  -m,--method <method>              Custom request method\n"
        "  -d,--data <data>                  HTTP POST data\n"
        "  -G,--get                          Put the post data in the URL and use GET\n"
        "  -F,--form <name=content>          Specify multipart MIME data\n"
        "     --form-string <name=string>    Specify multipart MIME data\n"
        "  -C,--cookie <data|filename>       Send cookies from string/file\n"
        "  -f,--cookie-file <filename>       Read or write cookies file <filename>\n"
        "  -s,--cookie-session               Start a new cookie session\n"
        "  -a,--append                       Append to target file when uploading\n"
        "  -T,--upload-file <file>           Transfer local FILE to destination\n"
        "  -k,--keepalive                    Enable TCP keep-alive\n"

        "  -n,--requests <requests>          Number of requests to perform\n"
        "  -t,--timelimit <seconds>          Seconds to max. to spend on benchmarking\n"
        "  -c,--concurrency <concurrency>    Number of multiple requests to make at a time\n"

        "  -w,--weight <weight>              URL weights\n"
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
    int c, ind = 0;
    idx_t *idxs;
    CURLM *multi;
    char fmt[64], *weight = NULL;

    memset(&cfg, 0, sizeof(cfg));

    cfg.concurrency = 10;

    while((c = getopt_long(argc, argv, options, OPTIONS, &ind)) != -1) {
        switch(c) {
            case 'V':
                printf("curl %s\n", LIBCURL_VERSION);
                exit(0);
                break;
            case 'i':
                cfg.info = true;
                break;
            case 'D':
                cfg.debug = optarg;
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
            case 'G': // get
                cfg.get = true;
                break;
            case FORM_STRING: // form-string
            case 'F': { // form
                if(cfg.formc >= sizeof(cfg.forms)/sizeof(cfg.forms[0])) {
                    fprintf(stderr, "form argument too many: %s\n", optarg);
                    break;
                }
                char *p = strchr(optarg, '=');
                if(p) {
                    bool is_file = (c == 'F' && *p == '@');
                    cfg.forms[cfg.formc].is_file = is_file;
                    cfg.forms[cfg.formc].name = strndup(optarg, p - optarg);
                    cfg.forms[cfg.formc].value = strdup(is_file ? p + 1 : p);
                    if(is_file && access(cfg.forms[cfg.formc].value, R_OK)) {
                        fprintf(stderr, "form file not exists: name: %s, value: %s\n", cfg.forms[cfg.formc].name, cfg.forms[cfg.formc].value);
                        cfg.formc++;
                        goto end;
                    }
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
            case 'f':
                cfg.cookie_file = optarg;
                break;
            case 's':
                cfg.cookie_session = true;
                break;
            case 'a':
                cfg.append = true;
                break;
            case 'T':
                cfg.upload_file = optarg;
                if(access(cfg.upload_file, R_OK)) {
                    fprintf(stderr, "upload file not exists: %s\n", cfg.upload_file);
                    goto end;
                }
                break;
            case 'k':
                cfg.keepalive = true;
                cfg.headers[cfg.headerc++] = "Connection: Keep-alive";
                cfg.headers[cfg.headerc++] = "Keep-Alive: timeout=20";
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

            case 'w': // weight
                weight = optarg;
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

    if(weight) {
        cfg.urlw = (int*) malloc(sizeof(int) * cfg.urlc);
        for(c=0; c<cfg.urlc; c++) {
            if(isdigit(*weight)) {
                cfg.urlw[c] = atoi(weight);
                if(cfg.urlw[c] <= 0) {
                    cfg.urlw[c] = 1;
                }
                while(isdigit(*weight)) weight ++;
                if(*weight && !isdigit(*weight)) weight ++;
            } else {
                cfg.urlw[c] = 1;
            }
        }
    }

    if(cfg.info) {
        printf("======== CONFIG INFO BEGIN ========\n");
        printf("debug: %s\n", cfg.debug);
        printf("verbose: %s\n", cfg.verbose ? "true" : "false");
        printf("headers: %d\n", cfg.headerc);
        for(c=0; c<cfg.headerc; c++) {
            printf("  %d => %s\n", c, cfg.headers[c]);
        }
        printf("head: %s\n", cfg.head ? "true" : "false");
        printf("method: %s\n", cfg.method);
        printf("data: %s\n", cfg.data);
        printf("get: %s\n", cfg.get ? "true" : "false");
        printf("forms: %d\n", cfg.formc);
        for(c=0; c<cfg.formc; c++) {
            printf("  %d => is_file: %s, name: %s, value: %s\n", c, cfg.forms[c].is_file ? "true" : "false", cfg.forms[c].name, cfg.forms[c].value);
        }
        printf("cookie: %s\n", cfg.cookie);
        printf("cookie_file: %s\n", cfg.cookie_file);
        printf("append: %s\n", cfg.append ? "true" : "false");
        printf("upload_file: %s\n", cfg.upload_file);
        printf("keepalive: %s\n", cfg.keepalive ? "true" : "false");
        printf("\n");
        printf("urls: %d\n", cfg.urlc);
        for(c=0; c<cfg.urlc; c++) {
            printf("  %d => [%d] %s\n", c, cfg.urlw ? cfg.urlw[c] : 1, cfg.urls[c]);
        }
        printf("\n");
        printf("requests: %d\n", cfg.requests);
        printf("timelimit: %d\n", cfg.timelimit);
        printf("concurrency: %d\n", cfg.concurrency);
        printf("========= CONFIG INFO END =========\n");
        goto end;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    idxs = (idx_t*) malloc(sizeof(idx_t) * cfg.concurrency);
    multi = curl_multi_init();

    memset(idxs, 0, sizeof(idx_t) * cfg.concurrency);

    if(cfg.debug) {
        snprintf(fmt, sizeof(fmt), "%%s/.debug-%%0%dd.log", (int) ceil(log10(cfg.concurrency+1)));
        if(*cfg.debug == '\0') cfg.debug = ".";
    }

    if(cfg.requests > 0 && cfg.concurrency > cfg.requests) cfg.concurrency = cfg.requests;

    for(c=0; c<cfg.concurrency; c++) {
        if(cfg.debug) asprintf(&idxs[c].logfile, fmt, cfg.debug, c+1);
        if(cfg.verbose) idxs[c].logfp = stderr;

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
        int still_running, msgs;
        CURL *curl;
        CURLMcode mc;
        struct CURLMsg *m;
        request_t *req;
        int code;
        int concurrency = cfg.concurrency;
        long int code0xx = 0, code1xx = 0, code2xx = 0, code3xx = 0, code4xx = 0, code5xx = 0, codex = 0;
        long int begin_reqs = cfg.concurrency, end_reqs = 0, prev_reqs = 0;
        long int prev_req_bytes = 0, prev_res_bytes = 0, prev_bug_bytes = 0;
        int times = 0;
        double req_times[10000];
        const long int req_timec = sizeof(req_times) / sizeof(req_times[0]);
        char bufs[3][32];

        memset(req_times, 0, sizeof(req_times));

        do {
            still_running = 0;
            mc = curl_multi_perform(multi, &still_running);

            if(mc) {
                fprintf(stderr, "curl_multi_perform error: %s\n", curl_multi_strerror(mc));
                break;
            }

            do {
                msgs = 0;
                m = curl_multi_info_read(multi, &msgs);
                if(m && (m->msg && CURLMSG_DONE)) {
                    curl = m->easy_handle;

                    code = 0;
                    req = NULL;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
                    curl_easy_getinfo(curl, CURLINFO_PRIVATE, &req);

                    curl_multi_remove_handle(multi, curl);
                    curl_easy_cleanup(curl);
                    curl = NULL;

                    if(code < 100) {
                        code0xx ++;
                    } else if(code < 200) {
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

                    req_times[end_reqs % req_timec] = microtime() - req->idx->time;
                    end_reqs ++;

                    if(req->idx->logfp) fprintf(req->idx->logfp, "%s * END %dst REQUEST - %lf\n", nowtime(), req->idx->reqs,  microtime() - req->idx->time);

                    if(is_running && (cfg.requests <= 0 || begin_reqs < cfg.requests) && (cfg.timelimit <= 0 || timelimit >= time(NULL))) {
                        begin_reqs ++;
                        curl_multi_add_handle(multi, make_curl(&cfg, req->idx));
                    } else {
                        concurrency --;
                    }

                    if(req->headers) curl_slist_free_all(req->headers);
                    if(req->form) curl_formfree(req->form);
                    if(req->fp_upload)  fclose(req->fp_upload);

                    free(req);
                    req = NULL;
                }
            } while(msgs);

            if(still_running) {
                mc = curl_multi_poll(multi, NULL, 0, 1000, NULL);
                if(mc) {
                    fprintf(stderr, "curl_multi_poll error: %s\n", curl_multi_strerror(mc));
                    break;
                }
            }

            if(is_timer || !concurrency) {
                is_timer = false;
                if(!is_running && isatty(1)) printf("\033[2K\r");

                double min = 0xffffffff, avg = 0, max = 0;
                for(c=0; c<req_timec && c<end_reqs; c++) {
                    if(req_times[c] < min) min = req_times[c];
                    avg += req_times[c];
                    if(req_times[c] > max) max = req_times[c];
                }
                if(c > 0) {
                	avg /= c;
                } else {
                	min = 0;
                }

                printf("times: %d, concurrency: %d, 0xx: %ld, 1xx: %ld, 2xx: %ld, 3xx: %ld, 4xx: %ld, 5xx: %ld, xxx: %ld, reqs: %ld/s, bytes: %s/%s/%s, min: %.1lfms, avg: %.1lfms, max: %.1lfms\n", ++times, concurrency, code0xx, code1xx, code2xx, code3xx, code4xx, code5xx, codex, end_reqs - prev_reqs, fsize(req_bytes - prev_req_bytes, bufs[0]), fsize(res_bytes - prev_res_bytes, bufs[1]), fsize(bug_bytes - prev_bug_bytes, bufs[2]), min * 1000.0f, avg * 1000.0f, max * 1000.0f);

                prev_reqs = end_reqs;
                prev_req_bytes = req_bytes;
                prev_res_bytes = res_bytes;
                prev_bug_bytes = bug_bytes;
            }
        } while(concurrency);

        // printf("begin_reqs: %d, end_reqs: %d\n", begin_reqs, end_reqs); // begin_reqs equals end_reqs
    }

    curl_multi_cleanup(multi);
    curl_global_cleanup();

    for(c=0; c<cfg.concurrency; c++) {
        if(idxs[c].logfile) {
            // unlink(idxs[c].logfile);
            free(idxs[c].logfile);
        }
        if(idxs[c].logfp) {
            fclose(idxs[c].logfp);
        }
    }
    free(idxs);

    if(cfg.urlw) {
        free(cfg.urlw);
    }

end:
    for(c=0; c<cfg.formc; c++) {
        free(cfg.forms[c].name);
        free(cfg.forms[c].value);
    }

    return 0;
}
