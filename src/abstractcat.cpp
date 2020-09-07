#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>


const char * source_name;
const char * destination_name;
int child = 0;

struct forward_info
{
    int source;
    int destination;
};

void * forward_thread(void * arg)
{
    forward_info * info = (forward_info *) arg;

    #define BUFSIZE 32768

    char buffer[BUFSIZE];

    while (true)
    {
        ssize_t bytes = recv(info->source, buffer, BUFSIZE, 0);
        if (bytes <= 0)
        {
            printf("[abstractcat] recv() failed: [%d][%s]\n", errno, strerror(errno));
            if (child) kill(child, SIGTERM);
            exit(-1);
        }
        // printf("[abstractcat] %d bytes received\n", bytes);
        char * i = buffer;
        while (i < buffer + bytes)
        {
            ssize_t sent = send(info->destination, i, buffer + bytes - i, 0);
            if (sent <= 0)
            {
                printf("[abstractcat] send() failed: [%d][%s]\n", errno, strerror(errno));
                if (child) kill(child, SIGTERM);
                exit(-1);
            }
            // printf("[abstractcat] %d bytes sent\n", sent);
            i += sent;
        }
    }
}

void * connect_thread(void * arg)
{
    bool is_abstract_destination = destination_name[0] == '@';
    int source_socket = (int) arg;
    int destination_socket = socket(AF_LOCAL, SOCK_STREAM, 0);
    struct sockaddr_un logaddr;
    socklen_t sun_len = is_abstract_destination ? 2 + strlen(destination_name) : sizeof(struct sockaddr_un);
    logaddr.sun_family = AF_UNIX;
    strcpy(logaddr.sun_path + (is_abstract_destination ? 1 : 0), destination_name + (is_abstract_destination ? 1 : 0));

    const int max_attempts = 20;
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        int result = connect(destination_socket, (struct sockaddr *) &logaddr, sun_len);
        if (result != 0)
        {
            printf("[abstractcat] connect() failed: [%d][%s]\n", errno, strerror(errno));
            if (attempt == max_attempts - 1)
            {
                if (child) kill(child, SIGTERM);
                exit(-1);
            }
            usleep(50000);
        }
        else
            break;
    }

    forward_info * info = (forward_info *) malloc(sizeof(forward_info)*2);
    info[0].source = source_socket;
    info[0].destination = destination_socket;
    info[1].source = destination_socket;
    info[1].destination = source_socket;

    pthread_t tid1;
    pthread_attr_t attr1;
    pthread_attr_init(&attr1);
    pthread_create(&tid1, &attr1, forward_thread, (void *)info);

    pthread_t tid2;
    pthread_attr_t attr2;
    pthread_attr_init(&attr2);
    pthread_create(&tid2, &attr2, forward_thread, (void *)(info+1));

    return NULL;
}

int run_process(int close_me, int argc, char ** argv)
{
    char ** last_env;
    for (last_env = argv; last_env < argc + argv; ++last_env)
    {
        if (!strchr(*last_env, '='))
            break;
    }

    const char ** envp = (const char **) malloc(sizeof(char *) * (last_env - argv + 1));
    memcpy(envp, argv, sizeof(char *) * (last_env - argv));
    envp[last_env - argv] = NULL;

    const char ** new_argv = (const char **) malloc(sizeof(char *) * (argc + argv - last_env));
    memcpy(new_argv, last_env, sizeof(char *) * (argc + argv - last_env));
    new_argv[argc + argv - last_env] = NULL;

    pid_t pid = fork();
    if (pid > 0)
        printf("[abstractcat] Process %d started.\n", pid);
    else if (pid == 0)
    {
        close(close_me);
        execve(new_argv[0], (char * const *) new_argv, (char * const *) envp);
        printf("[abstractcat] execve() failed [%d][%s]\n", errno, strerror(errno));
    }
    else
        printf("[abstractcat] fork() failed [%d][%s]\n", errno, strerror(errno));
    return pid;
}

int main(int argc, char ** argv)
{
    if (argc < 4)
    {
        printf("[abstractcat] Not enough arguments. :(\n");
        return -1;
    }

    source_name = argv[1];
    destination_name = argv[2];

    bool is_abstract_source = source_name[0] == '@';

    int server_socket = socket(AF_LOCAL, SOCK_STREAM, 0);
    struct sockaddr_un logaddr;
    socklen_t sun_len = is_abstract_source ? 2 + strlen(source_name) : sizeof(struct sockaddr_un);
    memset(&logaddr, 0, sun_len);
    logaddr.sun_family = AF_UNIX;
    logaddr.sun_path[0] = 0;
    strcpy(logaddr.sun_path + (is_abstract_source ? 1 : 0), source_name + (is_abstract_source ? 1 : 0));

    int flags = fcntl(server_socket, F_GETFL, 0);
    if (flags == -1)
    {
        printf("[abstractcat] fcntl() failed: [%d][%s]\n", errno, strerror(errno));
        return -1;
    }
    fcntl(server_socket, F_SETFL, flags | O_NONBLOCK);

    int result = bind(server_socket, (struct sockaddr *) &logaddr, sun_len);
    if (result != 0)
    {
        printf("[abstractcat] bind() failed: [%d][%s]\n", errno, strerror(errno));
        return -1;
    }

    result = listen(server_socket, 5);
    if (result != 0) {
        printf("[abstractcat] listen() failed: [%d][%s]\n", errno, strerror(errno));
        return -1;
    }

    if (argc > 4)
    {
        child = run_process(server_socket, argc - 4, &argv[4]);
    }

    const int max_connections = atoi(argv[3]);
    int connections = 0;
    int loops = 0;

    while (!feof(stdin))
    {
        ++loops;
        if (loops > 100)
        {
            printf("[abstractcat] Connection timeout\n");
            close(server_socket);
            if (child) kill(child, SIGTERM);
            return -1;
        }

        int client_socket = accept(server_socket, (struct sockaddr *) &logaddr, &sun_len);
        if (client_socket <= 0)
        {
            usleep(50000);
            continue;
        }

        ++connections;
        printf("[abstractcat] Accepted connection\n");

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_create(&tid, &attr, connect_thread, (void *)client_socket);

        if (connections == max_connections)
        {
            close(server_socket);
            while (!feof(stdin))
            {
                usleep(500000);
            }
            if (child) kill(child, SIGTERM);
            return 0;
        }
    }

    if (child) kill(child, SIGTERM);
    return 0;
}
