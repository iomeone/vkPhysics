#include <mutex>
#include <thread>
#include <chrono>
#include "log.hpp"
#include <string.h>
#include "meta.hpp"
#include <curl/curl.h>
#include "serialiser.hpp"
#include "allocators.hpp"
#include <condition_variable>

#define REQUEST_RESULT_MAX_SIZE 4096

static const char *meta_server_hostname = "meta.llguy.fun";

static std::thread meta_thread;

static std::condition_variable ready;
static std::mutex mutex;

// Shared state between job thread (doing requests), and main thread
static struct shared_t {
    request_t current_request_type;
    void *current_request_data;
    char *request_result;
    uint32_t request_result_size;
    bool doing_job;
} shared;

static bool requested_work;

#if 0
static request_t current_request_type;
static void *current_request_data;

// Result of the current request
static char *request_result;
static uint32_t request_result_size;
#endif

static CURL *curl;

// Buffer used for creating URLs, fields, etc...
static linear_allocator_t allocator;

static size_t s_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    uint32_t byte_count = nmemb * size;

    if (shared.request_result_size + byte_count < REQUEST_RESULT_MAX_SIZE) {
        memcpy(shared.request_result + shared.request_result_size, ptr, byte_count);
        shared.request_result_size += byte_count;
    }

    LOG_INFOV("META: Byte count: %d\n", byte_count);
    LOG_INFOV("META: Message: \n%s\n", shared.request_result);

    return byte_count;
}

#define REQUEST_MAX_SIZE 100

static serialiser_t s_fill_request(bool null_terminate = 0) {

    serialiser_t serialiser = {};
    serialiser.data_buffer = (uint8_t *)allocator.allocate(REQUEST_MAX_SIZE);
    serialiser.data_buffer_size = REQUEST_MAX_SIZE;

    serialiser.serialise_string("http://", 0);
    serialiser.serialise_string(meta_server_hostname, 0);
    serialiser.serialise_uint8('/');

    if (null_terminate)
        serialiser.serialise_uint8(0);

    return serialiser;
}

static void s_meta_thread() {
    for (;;) {
        allocator.clear();

        printf("META: Waiting on job...\n");

        std::unique_lock<std::mutex> lock (mutex);
        ready.wait(lock, [] { return shared.doing_job; });

        LOG_INFO("META: Started job\n");

        bool quit = 0;

        switch (shared.current_request_type) {
        case R_SIGN_UP: {
            request_sign_up_data_t *data = (request_sign_up_data_t *)shared.current_request_data;

            serialiser_t serialiser = s_fill_request();
            serialiser.serialise_string("api/register_user.php");
            curl_easy_setopt(curl, CURLOPT_URL, serialiser.data_buffer);

            serialiser_t fields = {};
            fields.data_buffer = (uint8_t *)allocator.allocate(REQUEST_MAX_SIZE);

            fields.data_buffer_size = REQUEST_MAX_SIZE;
            fields.serialise_string("username=", 0);
            fields.serialise_string(data->username, 0);
            fields.serialise_uint8('&');
            fields.serialise_string("password=", 0);
            fields.serialise_string(data->password, 1);

            // Fill post fields
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.data_buffer);
        } break;

        case R_QUIT: {
            quit = 1;
        } break;
        }

        if (quit) {
            break;
        }

        curl_easy_perform(curl);

        printf("\n\nMETA: Finished this job\n");;

        shared.doing_job = 0;

        lock.unlock();
    }
}

void begin_meta_client_thread() {
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    if (!curl) {
        LOG_ERROR("Failed to initialise CURL\n");
        exit(1);
    }

    curl_easy_setopt(curl, CURLOPT_URL, meta_server_hostname);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s_write_callback);
    curl_easy_setopt(curl, CURLOPT_POST, 1);

    shared.doing_job = 0;
    shared.request_result_size = 0;

    shared.request_result = FL_MALLOC(char, REQUEST_RESULT_MAX_SIZE);
    allocator.init(4096);

    meta_thread = std::thread(s_meta_thread);
}

char *check_request_finished(uint32_t *size, request_t *type) {
    if (requested_work) {
        std::unique_lock<std::mutex> lock (mutex);

        // If work was requested and the other thread isn't doing work
        if (!shared.doing_job) {
            LOG_INFO("MAIN: META thread stopped work\n");

            *size = shared.request_result_size;
            *type = shared.current_request_type;

            shared.request_result_size = 0;

            requested_work = 0;

            return shared.request_result;
        }
        else {
            return NULL;
        }
    }
    else {
        return NULL;
    }
}

void send_request(request_t request, void *data) {
    // This is not shared
    requested_work = 1;

    { // Push the request
        std::unique_lock<std::mutex> lock (mutex);
        shared.doing_job = 1;
        shared.current_request_type = request;
        shared.current_request_data = data;
    }

    ready.notify_one();
}

void join_meta_thread() {
    meta_thread.join();
}