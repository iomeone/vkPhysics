#include <mutex>
#include <cstdio>
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

static CURL *curl;

// Buffer used for creating URLs, fields, etc...
static linear_allocator_t allocator;

static size_t s_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    uint32_t byte_count = nmemb * size;

    if (shared.request_result_size + byte_count < REQUEST_RESULT_MAX_SIZE) {
        memcpy(shared.request_result + shared.request_result_size, ptr, byte_count);
        shared.request_result_size += byte_count;

        shared.request_result[shared.request_result_size + byte_count] = 0;
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

static void s_set_url(const char *path) {
    serialiser_t serialiser = s_fill_request();
    serialiser.serialise_string(path);
    curl_easy_setopt(curl, CURLOPT_URL, serialiser.data_buffer);
}

static void s_meta_thread() {
    for (;;) {
        allocator.clear();

        std::unique_lock<std::mutex> lock (mutex);
        ready.wait(lock, [] { return shared.doing_job; });

        LOG_INFO("META: Started job\n");
        memset(shared.request_result, 0, REQUEST_RESULT_MAX_SIZE);

        bool quit = 0;

        switch (shared.current_request_type) {
        case R_SIGN_UP: {
            request_sign_up_data_t *data = (request_sign_up_data_t *)shared.current_request_data;

            s_set_url("api/register_user.php");

            // Fill post fields
            char *fields = (char *)allocator.allocate(REQUEST_MAX_SIZE);
            sprintf(fields, "username=%s&password=%s", data->username, data->password);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
        } break;

        case R_AUTOMATIC_LOGIN: {
            request_automatic_login_t *data = (request_automatic_login_t *)shared.current_request_data;

            s_set_url("api/auto_login_user.php");

            char *fields = (char *)allocator.allocate(REQUEST_MAX_SIZE);
            sprintf(fields, "usertag=%d&userid=%d", data->usertag, data->userid);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
        } break;

        case R_LOGIN: {
            request_login_data_t *data = (request_login_data_t *)shared.current_request_data;

            s_set_url("api/login_user.php");

            char *fields = (char *)allocator.allocate(REQUEST_MAX_SIZE);
            sprintf(fields, "username=%s&password=%s", data->username, data->password);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
        } break;

        case R_REGISTER_SERVER: {
            request_register_server_t *data = (request_register_server_t *)shared.current_request_data;

            s_set_url("api/register_server.php");

            char *fields = (char *)allocator.allocate(REQUEST_MAX_SIZE);
            sprintf(fields, "servername=%s", data->server_name);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
        } break;

        case R_SERVER_ACTIVE: {
            request_server_active_t *data = (request_server_active_t *)shared.current_request_data;

            s_set_url("api/activate_server.php");

            char *fields = (char *)allocator.allocate(REQUEST_MAX_SIZE);
            sprintf(fields, "uid=%d", data->server_id);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
        } break;

        case R_SERVER_INACTIVE: {
            request_server_inactive_t *data = (request_server_inactive_t *)shared.current_request_data;

            s_set_url("api/deactivate_server.php");

            char *fields = (char *)allocator.allocate(REQUEST_MAX_SIZE);
            sprintf(fields, "uid=%d", data->server_id);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);

            quit = 1;
        } break;

        case R_AVAILABLE_SERVERS: {
            request_available_server_t *data = (request_available_server_t *)shared.current_request_data;

            s_set_url("api/available_servers.php");

            // There are no fields to set
        } break;

        case R_QUIT: {
            quit = 1;

            return;
        } break;
        }

        curl_easy_perform(curl);

        printf("META: Finished this job\n");;

#if 0
        if (quit) {
            break;
        }
#endif

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

            FL_FREE(shared.current_request_data);

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
