/*
    Copyright 2018-2020 Picovoice Inc.

    You may not use this file except in compliance with the license. A copy of the license is located in the "LICENSE"
    file accompanying this source.

    Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on
    an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
    specific language governing permissions and limitations under the License.
*/

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>

#include "pv_porcupine.h"

static volatile bool is_interrupted = false;

void interrupt_handler(int _) {
    (void) _;
    is_interrupted = true;
}

void set_ledring(char* pattern)
{
    char buffer[192];
    fprintf(stderr, "setting ledring to %s\n", pattern);
    sprintf(buffer, "/usr/local/bin/lipc-set-prop com.doppler.ledd Pattern %s", pattern);
    if (system(buffer))
        fprintf(stderr, "can't set ledring!\n");
}

void timed_send(uint32_t seconds, const char *ip, uint16_t port, FILE* fp)
{
    struct sockaddr_in cliaddr;
    struct timeval timeout;
    int clifd;
    int16_t *pcm = malloc(1280);

    if (!pcm)
    {
        fprintf(stderr, "can't malloc\n");
        goto error;
    }


    if((clifd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        fprintf(stderr, "can't socket\n");
        goto error;
    }

    memset(&cliaddr, 0, sizeof(struct sockaddr_in));
    cliaddr.sin_family = AF_INET;
    cliaddr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &cliaddr.sin_addr);

    timeout.tv_sec = 1;
    timeout.tv_usec = 500000;

    if (setsockopt (clifd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
    {
        fprintf(stderr, "can't setsockopt SO_RCVTIMEO\n");
        goto error;
    }

    if (setsockopt (clifd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
    {
        fprintf(stderr, "can't setsockopt SO_SNDTIMEO\n");
        goto error;
    }

    if (connect(clifd, (struct sockaddr *)&cliaddr, sizeof(cliaddr)))
    {
        fprintf(stderr, "can't connect\n");
        goto error;
    }

    int ret = fcntl(clifd , F_SETFL, O_NONBLOCK);
    printf("fcntl nonblock: %i\n", ret);

    struct timeval before;
    gettimeofday(&before, NULL);

    while (fread(pcm, 1, 1280, fp) == (size_t) 1280)
    {
        struct timeval after;
        ssize_t written;

        written = write(clifd, pcm, 1280);
        if (written < 0)
        {
            close(clifd);
            goto error;
        }

        gettimeofday(&after, NULL);
        if (after.tv_sec - before.tv_sec > seconds)
        {
            int i;

            /*  send some silence... */
            memset(pcm, 0, 1280);
            for (i = 0; i < 3; i++)
            {
                written = write(clifd, pcm, 1280);
                if (written < 0)
                {
                    close(clifd);
                    goto error;
                }
            }
            shutdown(clifd, SHUT_WR);
            sleep(1);
            close(clifd);
            return;
        }
    }

error:
    set_ledring("solid_red");
    sleep(2);
    return;
}

int main(int argc, char *argv[]) {
    if (argc != 7) {
        fprintf(stderr, "usage : %s library_path model_path keyword_path sensitivity ip port\n", argv[0]);
        exit(1);
    }

    signal(SIGINT, interrupt_handler);

    const char *library_path = argv[1];
    const char *model_path = argv[2];
    const char *keyword_path = argv[3];
    const float sensitivity = (float) atof(argv[4]);
    const char *ip = argv[5];
    const uint16_t port = atoi(argv[6]);

    void *porcupine_library = dlopen(library_path, RTLD_NOW);
    if (!porcupine_library) {
        fprintf(stderr, "failed to open library.\n");
        exit(1);
    }

    char *error = NULL;

    const char *(*pv_status_to_string_func)(pv_status_t) = dlsym(porcupine_library, "pv_status_to_string");
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "failed to load 'pv_status_to_string' with '%s'.\n", error);
        exit(1);
    }

    pv_status_t (*pv_porcupine_init_func)(const char *, int32_t, const char *const *, const float *, pv_porcupine_t **) =
            dlsym(porcupine_library, "pv_porcupine_init");
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "failed to load 'pv_porcupine_init' with '%s'.\n", error);
        exit(1);
    }

    void (*pv_porcupine_delete_func)(pv_porcupine_t *) = dlsym(porcupine_library, "pv_porcupine_delete");
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "failed to load 'pv_porcupine_delete' with '%s'.\n", error);
        exit(1);
    }

    pv_status_t (*pv_porcupine_process_func)(pv_porcupine_t *, const int16_t *, int32_t *) =
            dlsym(porcupine_library, "pv_porcupine_process");
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "failed to load 'pv_porcupine_process' with '%s'.\n", error);
        exit(1);
    }

    int32_t (*pv_porcupine_frame_length_func)() = dlsym(porcupine_library, "pv_porcupine_frame_length");
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "failed to load 'pv_porcupine_frame_length' with '%s'.\n", error);
        exit(1);
    }

    pv_porcupine_t *porcupine = NULL;
    pv_status_t status = pv_porcupine_init_func(model_path, 1, &keyword_path, &sensitivity, &porcupine);
    if (status != PV_STATUS_SUCCESS) {
        fprintf(stderr, "'pv_porcupine_init' failed with '%s'\n", pv_status_to_string_func(status));
        exit(1);
    }

    const int32_t frame_length = pv_porcupine_frame_length_func();

    int16_t *pcm = malloc(sizeof(int16_t) * frame_length);
    if (!pcm) {
        fprintf(stderr, "failed to allocate memory for audio buffer\n");
        exit(1);
    }

    FILE *wav = stdin;
    while (!is_interrupted) {
        if (fread(pcm, sizeof(int16_t), frame_length, wav) != (size_t) frame_length) {
            fprintf(stderr, "read bad number of frames\n");
            exit(1);
        }

        int32_t keyword_index = -1;
        status = pv_porcupine_process_func(porcupine, pcm, &keyword_index);
        if (status != PV_STATUS_SUCCESS) {
            fprintf(stderr, "'pv_porcupine_process' failed with '%s'\n", pv_status_to_string_func(status));
            exit(1);
        }

        if (keyword_index == 0) {
            // other LED ring animations can be found in /usr/local/etc/ledd
            set_ledring("zzz_comets");
            fprintf(stdout, "detected keyword %i\n", keyword_index);
            timed_send(3, ip, port, wav);
            set_ledring("off");
        }
    }

    free(pcm);
    pv_porcupine_delete_func(porcupine);
    dlclose(porcupine_library);

    return 0;
}
