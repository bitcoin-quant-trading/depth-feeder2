#include <arpa/inet.h>
#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "json/reader.h"

static size_t curlWriteFunc(const char *ptr, size_t size, size_t nmemb, std::string *stream)
{
    size_t len = size * nmemb;
    stream->append(ptr, len);
    return len;
}

static std::string curlHttp(const std::string &url, void *postData, const std::string &header, int timeOut)
{
    std::string content;

    CURL *curl = curl_easy_init();
    std::string useragent = "Mozilla/5.0 (Windows NT 6.1; WOW64; rv:13.0) Gecko/20100101 Firefox/13.0.1";

    curl_easy_setopt(curl, CURLOPT_URL, (std::string("https://") + url).c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, useragent.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeOut / 1000);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeOut / 1000);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, header.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (postData)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData);

    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteFunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &content);
    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK)
        content = std::string("");

    curl_slist_free_all(headers);

    curl_easy_cleanup(curl);

    return content;
}

static long long timestamp()
{
    struct timeval te;
    gettimeofday(&te, NULL);
    long long ms = te.tv_sec * 1000LL + te.tv_usec / 1000; // caculate milliseconds
    return ms;
}

#define DEST_PORT   8300
#define MAX_DEPTH   1024

static pthread_mutex_t mutex;
static long long lastTs[2];
static std::string url[2];
static int sock_fd;
static struct sockaddr_in addr_dest;
static double depthPack[MAX_DEPTH * 8 + 8];

static void *depthThread(void *arg)
{
    int id = *(int *)arg;
    bool isHuobi = (id == 1);

    pthread_detach(pthread_self());

    memset(depthPack, 0, sizeof(depthPack));

    long long ts = timestamp();
    std::string content = curlHttp(url[id], NULL, "", 1000);
    if (isHuobi) {
        if (content.find("\"btccny\"") == std::string::npos) {
            size_t p = content.find("btccny");
            if (p != std::string::npos)
                content.replace(p, 6, "\"btccny\"");
        }
    }

    pthread_mutex_lock(&mutex);

    Json::Reader reader;
    Json::Value value;
    if ((ts > lastTs[id]) && reader.parse(content, value)) {
        lastTs[id] = ts;
        Json::Value asks = value["asks"];
        Json::Value bids = value["bids"];
        int askNum = asks.size();
        int bidNum = bids.size();
        double ask, bid;
        int n = id * (MAX_DEPTH * 4 + 4);
        depthPack[n++] = (double)ts;
	depthPack[n++] = 0.0;
        depthPack[n++] = (double)askNum;
        depthPack[n++] = (double)bidNum;
        if (asks[0][0].asDouble() < asks[askNum - 1][0].asDouble()) {
            ask = asks[0][0].asDouble();
            for (int i = 0; i < askNum; i++) {
                depthPack[n++] = asks[i][0].asDouble();
                depthPack[n++] = asks[i][1].asDouble();
            }
        } else {
            ask = asks[askNum - 1][0].asDouble();
            for (int i = askNum - 1; i >= 0; i--) {
                depthPack[n++] = asks[i][0].asDouble();
                depthPack[n++] = asks[i][1].asDouble();
            }
        }
        bid = bids[0][0].asDouble();
        for (int i = 0; i < bidNum; i++) {
            depthPack[n++] = bids[i][0].asDouble();
            depthPack[n++] = bids[i][1].asDouble();
        }
        sendto(sock_fd, depthPack, sizeof(depthPack), 0, (const sockaddr *)&addr_dest, sizeof(addr_dest));
        printf("Depth sent: [%d] %lld, %.2lf, %.2lf\n", id, ts, ask, bid);
    }

    pthread_mutex_unlock(&mutex);

    return NULL;
}

static void sigroutine(int signo){
    pthread_t thread[2];
    int id[2] = {0, 1};
    switch (signo){
    case SIGALRM:
        pthread_create(&thread[0], NULL, depthThread, &id[0]);
        pthread_create(&thread[1], NULL, depthThread, &id[1]);
        break;
    }
}

int main(int argc, const char *argv[])
{
    struct itimerval tmval;

    if (argc < 2) {
        printf("Usage: %s [dest ip addr]\n", argv[0]);
        return 0;
    }

    url[0] = "www.okcoin.cn/api/v1/depth.do?symbol=btc_cny";
    url[1] = "api.huobi.com/staticmarket/depth_btc_json.js";

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&addr_dest, 0, sizeof(addr_dest));
    addr_dest.sin_family = AF_INET;
    addr_dest.sin_addr.s_addr = inet_addr(argv[1]);
    addr_dest.sin_port = htons(DEST_PORT);

    curl_global_init(CURL_GLOBAL_ALL);

    pthread_mutex_init(&mutex, NULL);

    tmval.it_interval.tv_sec = tmval.it_value.tv_sec = 0;
    tmval.it_interval.tv_usec = tmval.it_value.tv_usec = 110 * 1000;
    signal(SIGALRM, sigroutine);
    setitimer(ITIMER_REAL, &tmval, NULL);
    while (1)
        pause();

    pthread_mutex_destroy(&mutex);

    curl_global_cleanup();

    close(sock_fd);
}
