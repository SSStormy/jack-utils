#include "jack/jack.h"
#include "jack/types.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include <semaphore.h>
#include "errno.h"

/*
 * What this does:
 *  Disconnects system:capture_* when started
 *  Looks for port connections where the id_a is a system:capture_* port and if the id_b is not a preset name, it
 *    disconnects the port
 */

#define ARRAY_SIZE(__what) (sizeof(__what) / sizeof(*__what))
#define Foreach(__ARRAY, __ITERATOR) for(int __ITERATOR = 0; __ITERATOR < ARRAY_SIZE(__ARRAY); __ITERATOR++)

static const char* mic_ports[] = {
    "system:capture_1",
    "system:capture_2",
};

static const char* acceptable_mic_connections[] = {
    "ardour:Mic/audio_in 1",
    "ardour:Mic/audio_in 2",
    "ardour:Mic loopback/audio_in 1",
    "ardour:Mic loopback/audio_in 2"
};

typedef struct disconnect_pipe_t {
    jack_port_id_t buffer[256];
    int buffer_watermark;

    sem_t read_semaphore;
    sem_t write_semaphore;
} disconnect_pipe;

int has_elements(disconnect_pipe *pipe) {
    int status = sem_trywait(&pipe->read_semaphore);
    return status == 0;
}

jack_port_id_t unqueue_element(disconnect_pipe *pipe) {
    jack_port_id_t value = pipe->buffer[--pipe->buffer_watermark];
    printf("unqueue: %d %d\n", pipe->buffer_watermark, value);

    sem_post(&pipe->write_semaphore);
    return value;
}

void queue_element(disconnect_pipe *pipe, jack_port_id_t id) {
    sem_wait(&pipe->write_semaphore);

    pipe->buffer[pipe->buffer_watermark++] = id;
    printf("add to queue: %d %d\n", pipe->buffer_watermark - 1, id);

    sem_post(&pipe->read_semaphore);
    sem_post(&pipe->write_semaphore);
}

jack_client_t *client;
disconnect_pipe id_pipe;

volatile jack_port_id_t disconnect_port;
volatile int should_disconnect_port = 0;

void try_reconnect_mic() {
    Foreach(acceptable_mic_connections, index) {
        const mic_port_index = index % ARRAY_SIZE(mic_ports);
        const char* a = mic_ports[mic_port_index];

        const char* b = acceptable_mic_connections[index];
        printf("reconnect: %s %s\n", a, b);
        jack_connect(client, a, b);
    }
}

void on_port_connect(jack_port_id_t id_a, jack_port_id_t id_b, int is_connected, void *arg) {
    jack_port_t *port_a = jack_port_by_id(client, id_a);
    jack_port_t *port_b = jack_port_by_id(client, id_b);

    const char *port_a_name = jack_port_name(port_a);

    int is_mic = 0;
    Foreach(mic_ports, index) {

        if(strcmp(port_a_name, mic_ports[index]) == 0) {
            is_mic = 1;
            break;
        }
    }

    if(!is_mic) {
        return;
    }

    const char *port_b_name = jack_port_name(port_b);

    printf("connection: %s %s\n", port_a_name, port_b_name);

    int should_disconnect = 1;
    Foreach(acceptable_mic_connections, index) {
        const char* cur = acceptable_mic_connections[index];

        int cmp = strcmp(cur, port_b_name);
//        printf("compare: %d %s %s\n", cmp, cur, port_b_name);

        if(cmp == 0) {
            should_disconnect = 0;
            break;
        }
    }

    if(should_disconnect) {
        queue_element(&id_pipe, id_a);
    }
}

void disconnect_all_port_connections(const char *port_name) {
    jack_port_t *port = jack_port_by_name(client, port_name);
    if(!port) {
        printf("Failed to find port with name %s\n", port_name);
        return;
    }

    jack_port_disconnect(client, port);
}

void init_semaphore(sem_t *sem, int val) {
    int status = sem_init(sem, 0, val);
    if(status != 0) {
        printf("Failed to initialize semaphore. Code: %d\n", status);
        exit(1);
    }
}

int main() {

    id_pipe.buffer_watermark = 0;
    init_semaphore(&id_pipe.read_semaphore, 0);
    init_semaphore(&id_pipe.write_semaphore, 1);

    {
        jack_status_t status;
        client = jack_client_open("UTIL: fucking microphone", JackNullOption, &status);

        if(!client) {
            printf("Failed to create JACK client.\n");
            exit(1);
        }
    }


    {
        jack_set_port_connect_callback(client, on_port_connect, 0);
        jack_activate(client); 
    }


    {
        Foreach(mic_ports, name_index) {
            disconnect_all_port_connections(mic_ports[name_index]);
        }

        try_reconnect_mic();
    }
    
    while(1) {
        while(has_elements(&id_pipe)) {
            jack_port_id_t id = unqueue_element(&id_pipe);

            jack_port_disconnect(client, jack_port_by_id(client, id));
            try_reconnect_mic();
        }

        sleep(1);
    }

    {
        sem_destroy(&id_pipe.write_semaphore);
        sem_destroy(&id_pipe.read_semaphore);
        jack_deactivate(client);
        jack_client_close(client);
    }
}
