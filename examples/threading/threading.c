#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    // TODO: wait, obtain mutex, wait, release mutex as described by thread_data structure
    // hint: use a cast like the one below to obtain thread arguments from your parameter
    struct thread_data* thread_func_args = (struct thread_data *) thread_param;

    // 1. Wait before obtaining mutex
    // usleep takes microseconds, so multiply milliseconds by 1000
    usleep(thread_func_args->wait_to_obtain_ms * 1000);

    // 2. Obtain mutex
    int rc = pthread_mutex_lock(thread_func_args->mutex);
    if (rc != 0) {
        ERROR_LOG("Failed to lock mutex");
        thread_func_args->thread_complete_success = false;
        return thread_param;
    }

    // 3. Hold (wait) within critical section
    usleep(thread_func_args->wait_to_release_ms * 1000);

    // 4. Release mutex
    rc = pthread_mutex_unlock(thread_func_args->mutex);
    if (rc != 0) {
        ERROR_LOG("Failed to unlock mutex");
        thread_func_args->thread_complete_success = false;
        return thread_param;
    }

    // If we got here, everything succeeded
    thread_func_args->thread_complete_success = true;

    return thread_param;
}

bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex, int wait_to_obtain_ms, int wait_to_release_ms)
{
    /**
     * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
     * using threadfunc() as entry point.
     *
     * return true if successful.
     *
     * See implementation details in threading.h file comment block
     */

    // 1. Allocate memory for thread_data
    struct thread_data *data = (struct thread_data *)malloc(sizeof(struct thread_data));
    if (data == NULL) {
        ERROR_LOG("Failed to allocate memory for thread_data");
        return false;
    }

    // 2. Setup the structure data
    data->mutex = mutex;
    data->wait_to_obtain_ms = wait_to_obtain_ms;
    data->wait_to_release_ms = wait_to_release_ms;
    data->thread_complete_success = false; // Default to false until thread finishes

    // 3. Create the thread
    // We pass 'data' as the argument to threadfunc
    int rc = pthread_create(thread, NULL, threadfunc, data);
    
    if (rc != 0) {
        ERROR_LOG("Failed to create thread");
        // Clean up memory since thread creation failed
        free(data); 
        return false;
    }

    DEBUG_LOG("Thread started successfully");
    return true;
}

