#if !defined(_GNU_SOURCE)
        #define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <errno.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <execinfo.h>
#include <time.h>
#include <fcntl.h>
#include <semaphore.h>

#define wrap_perror(message) fputs("\x1b[1;31m", stderr); perror(message); fputs("\x1b[0m\n", stderr);
#define print_error(message) fputs("\x1b[1;31m", stderr); fputs(message, stderr); fputs("\x1b[0m\n", stderr);

// Для удобства вывода ip
#define ip_to_bytes(ip) ip >> 3 * 8, (ip << 1 * 8) >> 3 * 8, (ip << 2 * 8) >> 3 * 8, (ip << 3 * 8) >> 3 * 8

#define CHILD_NEED_WORK 1
#define CHILD_NEED_TERMINATE 2

#define ERRNO_FLAG 1
#define INTERACTIVE_FLAG 2
#define MUTEX_DESTROY_FLAG 4

#define FD_LIMIT 10*1024

struct client
{
    struct sockaddr_in* addr;
    char* name;
    int* socket;
};

pthread_t thread_connecting;
char* pid_file;

static void signal_error(int sig, siginfo_t *si, void *ptr);

void set_fd_limit(int MaxFd);

int set_pid_file(char* filename);
void delete_pid_file(char* filename);
int check_pid_file(char*filename);

void* listening(void* arg);
void* connecting(void* arg);

int init_work_thread();
int destroy_work_thread();

int my_daemon();
int monitor();

void write_log(char* filename, char* msg, int flags);

//Запись коротких сообщений в syslog (для функции write_log)
void write_syslog(char* msg)
{
    openlog("my_server", LOG_PID, LOG_USER);
    syslog(LOG_ERR, "%s", msg);
    closelog();
}

//Печать сообщений в лог файл
/* Хранит путь к логу самостоятельно, т.е. каждый раз указывать flename не нужно, достаточно вызвать
   первый раз с указанием пути, либо вызвать с указанием пути, но msg = NULL (так же можно изменять),
   в прочих случаях (когда путь к логу уже сохранён), можно присвоить filename NULL.
   Флаги:
   ERRNO_FLAG           - Вывод переменной errno
   INTERACTIVE_FLAG     - Вывод на консоль (интерактивный режим) (аналогично filename сохраняется,
                          для установки без вывода нужно задать filename=NULL и msg=NULL)
   MUTEX_DESTROY_FLAG   - Уничтожение мьютекса, производить при выходе из программы.

   Сохранение некоторых параметров необходимо при обработке ошибок (signal_error), дабы избавиться от глобальных
   переменных. 
*/
void write_log(char* filename, char* msg, int flags)
{
    static char* log_file_name = NULL;
    static int interactive = 0;
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    
    // Удаление мьютекса (происходит при закрытии приложения)
    if(flags & MUTEX_DESTROY_FLAG)
    {
        if(pthread_mutex_destroy(&mutex) != 0)
            write_syslog("[Write Log] Mutex destroy failed");
        return;
    }

    //Блокируем критическую область (необходимо из-за того, что переменные, которые мы можем изменить - статические)
    if(pthread_mutex_lock(&mutex) != 0)
    {
        write_syslog("[Write Log] Mutex lock failed");
        return;
    }

    /* Если это первое открытие с указанным полем filename, или вызов без поля msg, но с filename,
       значит нужно запомнить filename */
    if(filename != NULL && (log_file_name == NULL || msg == NULL))
    {
        log_file_name = filename;
        //Если при этом и msg указанно, то выводим сообщение, иначе - выход.
        if(msg == NULL)
        {
            //Выход из критической области.
            if(pthread_mutex_unlock(&mutex) != 0)
                write_syslog("[Write Log] Mutex unlock failed");
            return;
        }
    }

    //Если это пытаются задать только флаги
    if(filename == NULL && msg == NULL)
    {
        //Пока для запоминания доступен только один флаг
        interactive = ((flags & INTERACTIVE_FLAG) == INTERACTIVE_FLAG);
        //Т.к. msg не задан, то и выводить нечего - выход.
        //Выход из критической области.
        if(pthread_mutex_unlock(&mutex) != 0)
            write_syslog("[Write Log] Mutex unlock failed");
        return;
    }
    
    //Файл, в который будем выводить
    FILE* file;

    //Если запускались в интерактивном режиме
    if((flags & INTERACTIVE_FLAG) || interactive)
    {
        //Запомним это на будущее
        interactive = 1;
        // Выводим на консоль
        file = stdout;
    }
    //Если запускались в режиме демона
    else
    {
        //"Вспоминаем" имя файла
        if(filename == NULL && log_file_name != NULL)
            file = fopen(log_file_name, "a");
        //Если указано другое место хранения логов
        else if(filename != NULL)
            file = fopen(filename, "a");
        //Если имя файла всё ещё не задано, то вывод будем производить в syslog
        if(file == NULL)
        {
            //Назовёмся "my_server" и укажем PID
            openlog("my_server", LOG_PID, LOG_USER);

            //Если был флаг вывода сообщения errno
            if(flags & ERRNO_FLAG)
                syslog(LOG_ERR, "%s errno: %s", msg, strerror(errno));
            else
                syslog(LOG_ERR, "%s", msg);

            //Выход из критической области.
            if(pthread_mutex_unlock(&mutex) != 0)
                syslog(LOG_ERR, "[Write Log] Mutex unlock failed");
            closelog();
            return;
        }
    }

    //Если с файлом всё в порядке (пока)

    //Получаем текущее время
    time_t rawtime = time(NULL);
    char* date = asctime(localtime(&rawtime));

    /* Заменяем перевод строки на символ конца строки, мы сами решаем когда
       вствлять перевод строки */
    date[strrchr(date, '\n') - date] = '\0';

    //В зависимости от флага вывода сообщения errno пишем дату, сообщение и, возможно, ошибку.
    if(flags & ERRNO_FLAG)
    {
        if(fprintf(file, "%s %s errno: %s\n", date, msg, strerror(errno)) < 0)
        {
            //Если не удаётся писать в файл - пишем в syslog
            openlog("my_server", LOG_PID, LOG_USER);
        
            syslog(LOG_ERR, "%s errno: %s", msg, strerror(errno));

            //Если запущен в режиме демона, значит открывал файл, который надо бы закрыть.
            if(!interactive)
            {
                if(fclose(file) == EOF)
                {
                    syslog(LOG_ERR, "[Write Log] Failed close file");
                }
            }

            //Выход из критической области.
            if(pthread_mutex_unlock(&mutex) != 0)
                syslog(LOG_ERR, "[Write Log] Mutex unlock failed");
            closelog();
            return;
        }
    }
    else
    {
        if(fprintf(file, "%s %s\n", date, msg) < 0)
        {   
            //Если не удаётся писать в файл - пишем в syslog
            openlog("my_server", LOG_PID, LOG_USER);
        
            syslog(LOG_ERR, "%s", msg);

            //Если запущен в режиме демона, значит открывал файл, который надо бы закрыть.
            if(!interactive)
            {
                if(fclose(file) == EOF)
                {
                    syslog(LOG_ERR, "[Write Log] Failed close file");
                }
            }

            //Выход из критической области.
            if(pthread_mutex_unlock(&mutex) != 0)
                syslog(LOG_ERR, "[Write Log] Mutex unlock failed");
            closelog();
            return;
        }
    }

    //Если запущен в режиме демона, значит открывал файл, который надо бы закрыть.
    if(!interactive)
    {
        /* Если файл не закрывается, то возможно, что сообщение и не запишется,
           механизм отложенной записи linux в действии. */
        if(fclose(file) == EOF)
        {
            //Если не удаётся писать в файл - пишем в syslog
            openlog("my_server", LOG_PID, LOG_USER);

            syslog(LOG_ERR, "[Write Log] Failed close file");

            if(flags & ERRNO_FLAG)
                syslog(LOG_ERR, "%s errno: %s", msg, strerror(errno));
            else
                syslog(LOG_ERR, "%s", msg);

            //Выход из критической области.
            if(pthread_mutex_unlock(&mutex) != 0)
                syslog(LOG_ERR, "[Write Log] Mutex unlock failed");
            closelog();
            return;
        }
    }
    //Выход из критической области.
    if(pthread_mutex_unlock(&mutex) != 0)
    {
        write_syslog("[Write Log] Mutex unlock failed");
        write_syslog(msg);
        return;
    }
}


/* Установка лимита на дескрипторы. По умолчанию это значение = 1024, чего может не хватить серверу. */
void set_fd_limit(int MaxFd)
{
    struct rlimit lim;

    // зададим текущий лимит на кол-во открытых дискриптеров
    lim.rlim_cur = MaxFd;
    // зададим максимальный лимит на кол-во открытых дискриптеров
    lim.rlim_max = MaxFd;

    // установим указанное кол-во
    if(setrlimit(RLIMIT_NOFILE, &lim) == -1)
    {
        write_log(NULL, "[Set FD limit] Error: ", ERRNO_FLAG);
    }
}

/* Запись PID в файл. */
int set_pid_file(char* filename)
{
    FILE* f;
    f = fopen(filename, "w");
    if (f != NULL)
    {
        if(fprintf(f, "%u", getpid()) < 0)
        {
            write_log(NULL, "[PID Write] Error: writing PID", 0);
            return 1;
        }
        if(fclose(f) == EOF)
        {
            write_log(NULL, "[PID_FILE] Error: Failed close file", ERRNO_FLAG);
        }
    }
    else
    {
        write_log(NULL, "[PID Write] Error: opening PID file", ERRNO_FLAG);
        return 2;
    }
    return 0;
}

/* Удаление PID файла */
void delete_pid_file(char* filename)
{
    int status = unlink(filename);
    if(status != 0)
    {
        write_log(NULL, "[MONITOR] Error: Failed to delete a PID file", ERRNO_FLAG);
        struct stat buf;
        status = stat(filename, &buf);
        /* Если удалось получить информацию о файле, значит он всё ещё существует,
           рекомендуем пользователю удалить этот файл самотстоятельно. */
        if(status == 0)
        {
            write_log(NULL, "[MONITOR] We recommend you to remove PID file yourself. Path:", 0);
            write_log(NULL, filename, 0);
        }
        /* Если errno == ENOENT, значит файл не существует,
           в противном случае просто кидаем ошибку */
        else if(errno != ENOENT)
        {
            write_log(NULL, "[MONITOR] Error: Failed to check PID file status", ERRNO_FLAG);
        }
    }
}

/* Проверка существования PID файла,
   если файл существует, вернёт 0, иначе - 1 */
int check_pid_file(char* filename)
{
    struct stat buf;
    stat(filename, &buf);
    if(errno == ENOENT)
        return 0;
    else
        return 1;
}

/* Прослушивание сообщений от клиента.
   Для каждого клиента в отдельном потоке работает свой экземпляр этой функции. 
   В качестве параметра ожидает struct client**.*/
void* listening(void* arg)
{
    sigset_t sigset;

    if(sigemptyset(&sigset) != 0)
    {
        write_log(NULL, "[LISTENING] Error: Failed to create empty blocker of a signal handler", ERRNO_FLAG);
    }
    // блокируем сигналы которые будем ожидать

    // сигнал остановки процесса пользователем
    if(sigaddset(&sigset, SIGQUIT) != 0)
    {
        write_log(NULL, "[LISTENING] Error: Failed to add SIGQUIT to blocker", ERRNO_FLAG);
    }
    // сигнал для остановки процесса пользователем с терминала
    if(sigaddset(&sigset, SIGINT) != 0)
    {
        write_log(NULL, "[LISTENING] Error: Failed to add SIGINT to blocker", ERRNO_FLAG);
    }
    // сигнал запроса завершения процесса
    if(sigaddset(&sigset, SIGTERM) != 0)
    {
        write_log(NULL, "[LISTENING] Error: Failed to add SIGTERM to blocker", ERRNO_FLAG);
    }
    if(sigprocmask(SIG_BLOCK, &sigset, NULL) != 0)
    {
        write_log(NULL, "[LISTENING] Error: Failed to apply blocker of a signal handler", ERRNO_FLAG);
    }
    //Буфер для хранения части сообщения
    char buf[64];

    //Всё сообщение целиком
    char* full_message = NULL;
    // Длинна сообщения
    size_t size_message = 0;
    while(1)
    {
        //Чтение пришедшего сообщения (блокируется)
        ssize_t bytes_read = recv(*((struct client*)arg)->socket, buf, sizeof(buf), 0);
        if(bytes_read <= 0) //Разрыв соединения
        {
            free(full_message);

            //Пишем об этом в лог, с указанием ip и порта клиента
            uint32_t ip = ntohl(((struct client*)arg)->addr->sin_addr.s_addr);
            uint16_t port = ntohs(((struct client*)arg)->addr->sin_port);
            
            /* Максимальная длинна строки с ip адресом - 15 (255.255.255.255)
               Максимальная длинна строки с портом - 6 (:65535)
               1 символ конца строки. */
            char* str = (char*)malloc(sizeof(char)*(15+6+strlen("[] Connection lost")+1));

            if(str == NULL)
            {
                write_log(NULL, "[LISTENING] Connection lost", 0);
                write_log(NULL, "[LISTENING] Error: Failed to allocate memory", 0);
                break;
            }
            if(sprintf(str, "[%hhu.%hhu.%hhu.%hhu:%hu] Connection lost", ip_to_bytes(ip), port) < 0)
            {
                write_log(NULL, "[LISTENING] Connection lost", 0);
                write_log(NULL, "[LISTENING] Error: sprintf failed", 0);
            }
            else
            {
                write_log(NULL, str, 0);
            }
            free(str);
            break;
        }

        if(full_message != NULL)
        {
            char* reallocated_memory = (char*)realloc(full_message, sizeof(char)*(size_message+bytes_read+1));
            if(reallocated_memory == NULL)
            {
                write_log(NULL, "[LISTENING] Error reallocating memory", 0);
                write_log(NULL, "[LISTENING] Saved string:", 0);
                write_log(NULL, full_message, 0);
            }
            else
            {
                full_message = reallocated_memory;
                memcpy(full_message+size_message, buf, (size_t)bytes_read);
                size_message += (size_t)bytes_read;

                //Необходим для вывода строки в случае ошибки
                full_message[size_message] = '\0';
            }
        }
        else
        {
            uint32_t ip = ntohl(((struct client*)arg)->addr->sin_addr.s_addr);
            uint16_t port = ntohs(((struct client*)arg)->addr->sin_port);
            /* Максимальная длинна строки с ip адресом - 15 (255.255.255.255)
               Максимальная длинна строки с портом - 6 (:65535)
               1 байт - конец строки, добавляемый sprintf. */
            full_message = (char*)malloc(sizeof(char)*(bytes_read+15+6+strlen("[] : ")+1));
            if(full_message == NULL)
            {
                write_log(NULL, "[LISTENING] Error: Failed to allocate memory", 0);
            }
            else
            {
                int write_len = sprintf(full_message, "[%hhu.%hhu.%hhu.%hhu:%hu] : ", ip_to_bytes(ip), port);
                if(write_len < 0)
                {
                    write_log(NULL, "[LISTENING] Error: sprintf failed", 0);
                    free(full_message);
                    full_message = NULL;
                    size_message = 0;
                }
                else
                {
                    // -1 для перезаписи '\0' (символа конца строки), добавленного sprintf'ом.
                    memcpy(full_message+write_len-1, buf, (size_t)bytes_read);
                    size_message = (size_t)(bytes_read + write_len-1);

                    //Необходим для вывода строки в случае ошибки
                    full_message[size_message] = '\0';
                }
            }
        }
        //Если клиент прислал символ конца строки - это конец данного сообщения, вывод.
        if(full_message != NULL && full_message[size_message-1] == '\0')
        {
            write_log(NULL, full_message, 0);

            free(full_message);
            full_message = NULL;
            size_message = 0;
        }
    }
    free(full_message);

    if(close(*((struct client*)arg)->socket) < 0)
    {
        write_log(NULL, "[LISTENING] Error: Failed to close socket", ERRNO_FLAG);
    }

    free(((struct client*)arg)->socket);
    free(((struct client*)arg)->addr);
    free(((struct client*)arg)->name);

    free(arg);

    pthread_exit(NULL);
}


/* Установка соединения с новым клиентом. */
void* connecting(void* arg)
{
    //Сокет для приёма входящих подключений.
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    //помечаем сокет неблокируемым
    fcntl(sock, F_SETFL, O_NONBLOCK);
    if(sock == -1)
    {
        write_log(NULL, "[Server] Error: creating socket", ERRNO_FLAG);
        kill(getpid(), SIGTERM);
        pthread_exit(NULL);
    }

    //Прослушиваем TCP, порт 13000.
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(13000);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(sock, (struct sockaddr *) &addr, sizeof(addr)) != 0)
    {
        write_log(NULL, "[Server] Error: bind", ERRNO_FLAG);
        if(close(sock) < 0)
        {
            write_log(NULL, "[Server] Error: Failed to close bind socket", ERRNO_FLAG);
        }
        kill(getpid(), SIGTERM);
        pthread_exit(NULL);
    }
    if (listen(sock, SOMAXCONN) != 0)
    {
        write_log(NULL, "[Server] Error: listen", ERRNO_FLAG);
        if(close(sock) < 0)
        {
            write_log(NULL, "[Server] Error: Failed to close bind socket", ERRNO_FLAG);
        }
        kill(getpid(), SIGTERM);
        pthread_exit(NULL);
    }
    write_log(NULL, "[Server] Started", 0);

    //Цикл обработки входящих соединений
    while(1)
    {

        int* client_sock = (int*)malloc(sizeof(int));
        if(client_sock == NULL)
        {
            write_log(NULL, "[Server] Error: Failed allocate memory", 0);
            continue;
        }
        struct sockaddr* client_addr = (struct sockaddr*)malloc(sizeof(struct sockaddr));
        if(client_addr == NULL)
        {
            write_log(NULL, "[Server] Error: Failed allocate memory", 0);
            free(client_sock);
            continue;
        }
        socklen_t client_addr_len = sizeof(*client_addr);

        /* Ниже вызов select, который вернёт значение, большее 0, только если в потоке
           будет что читать (т.е. приёт запрос на подключение), благодаря этому вызов
           accept не заблокирует поток и даст спокойно себя завершить*/
        {
            int sel_ret = 0;
            while(sel_ret == 0)
            {
                fd_set set;
                struct timeval tv;
                FD_ZERO(&set);
                FD_SET(sock, &set);
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                sel_ret = select(sock+1, &set, NULL, NULL, &tv);
                pthread_testcancel();
            }
            if(sel_ret == -1)
            {
                write_log(NULL, "[Server] Error: select", ERRNO_FLAG);
                if(close(sock) < 0)
                {
                    write_log(NULL, "[Server] Error: Failed to close bind socket", ERRNO_FLAG);
                }
                free(client_sock);
                free(client_addr);
                kill(getpid(), SIGTERM);
                pthread_exit(NULL);
            }
        }
        //Приём подключения 
        *client_sock = accept(sock, client_addr, &client_addr_len);
        if(*client_sock < 0)
        {
            write_log(NULL, "[Server] Error: accept", ERRNO_FLAG);
            if(close(sock) < 0)
            {
                write_log(NULL, "[Server] Error: Failed to close bind socket", ERRNO_FLAG);
            }
            free(client_sock);
            free(client_addr);
            kill(getpid(), SIGTERM);
            pthread_exit(NULL);
        }

        struct client* new_client = (struct client*)malloc(sizeof(struct client));
        if(new_client == NULL)
        {
            write_log(NULL, "[Server] Error: Failed allocate memory", 0);
            if(close(*client_sock) < 0)
            {
                write_log(NULL, "[Server] Error: Failed to close client's socket", ERRNO_FLAG);
            }
            free(client_sock);
            free(client_addr);
            continue;
        }
        new_client->addr = (struct sockaddr_in*)client_addr;
        new_client->socket = client_sock;
        new_client->name = NULL;

        {
            uint32_t ip = ntohl(((struct sockaddr_in *)client_addr)->sin_addr.s_addr);
            uint16_t port = ntohs(((struct sockaddr_in *)client_addr)->sin_port);
            /* Максимальная длинна строки с ip адресом - 15 (255.255.255.255)
               Максимальная длинна строки с портом - 6 (:65535)
               1 байт - конец строки, добавляемый sprintf */
            char* str = (char*)malloc(sizeof(char)*(15+6+strlen("[Server] new client "+1)));
            if(str == NULL)
            {
                write_log(NULL, "[Server] Error: Failed allocate memory", 0);
            }
            if(sprintf(str, "[Server] new client %hhu.%hhu.%hhu.%hhu:%hu", ip_to_bytes(ip), port) < 0)
            {
                write_log(NULL, "[Server] Error: sprintf failed", 0);
            }
            write_log(NULL, str, 0);
            free(str);
        }

        //Создание нового потока
        pthread_t thread;
        if(pthread_create(&thread, NULL, listening, new_client) == EAGAIN)
        {
            write_log(NULL, "[Server] Error: a system-imposed limit on the number of threads was encountered", 0);
            if(close(*client_sock) < 0)
            {
                write_log(NULL, "[Server] Error: Failed to close client's socket", ERRNO_FLAG);
            }
            free(client_sock);
            free(client_addr);
            free(new_client);
            continue;
        }
        pthread_detach(thread);
    }
}

//Обработка некоторых ошибок (SIGFPE, SIGILL, SIGSEGV, SIGBUS)
static void signal_error(int sig, siginfo_t *si, void *ptr)
{
    void* ErrorAddr;
    void* Trace[17];
    int TraceSize;
    char** Messages;

    //Запишем в лог что за сигнал пришел
    {
        char* str = (char*)malloc(sizeof(char)*(strlen("[DAEMON] Signal: , Addr: 0x") +
                                                strlen(strsignal(sig))+17));
        if(str == NULL)
        {
            write_log(NULL, "[Err Handler] Error: Failed allocate memory", 0);
        }
        else
        {
            if(sprintf(str, "[DAEMON] Signal: %s, Addr: 0x%.16X", strsignal(sig), si->si_addr) < 0)
            {
                write_log(NULL, "[Err Handler] Error: sprintf failed", 0);
            }
            else
            {
                write_log(NULL, str, 0);
            }
            free(str);
        }
    }
    
       
    #if __WORDSIZE == 64 // если дело имеем с 64 битной ОС
        //Получим адрес инструкции которая вызвала ошибку
        ErrorAddr = (void*)((ucontext_t*)ptr)->uc_mcontext.gregs[REG_RIP];
    #else
        //Получим адрес инструкции которая вызвала ошибку
        ErrorAddr = (void*)((ucontext_t*)ptr)->uc_mcontext.gregs[REG_EIP];
    #endif
    
    //Произведем backtrace чтобы получить весь стек вызовов
    TraceSize = backtrace(Trace+1, 16);
    Trace[0] = ErrorAddr;

    //Получим расшифровку трасировки
    Messages = backtrace_symbols(Trace, TraceSize);
    if(Messages == NULL)
    {
        write_log(NULL, "[Err Handler] Error: backtrace", 0);
    }
    else
    {
        write_log(NULL, "== Error's place ==", 0);
        write_log(NULL, Messages[0], 0);
        
        write_log(NULL, "== Backtrace ==", 0);

        for (int x = 0; x < TraceSize; x++)
        {
            write_log(NULL, Messages[x], 0);
        }
        
        write_log(NULL, "== End Backtrace ==", 0);
        free(Messages);
    }

    write_log(NULL, "[DAEMON] Stopped", 0);
    
    //Остановим все рабочие потоки
    destroy_work_thread();
    
    //Завершим процесс с кодом требующим перезапуска
    exit(CHILD_NEED_WORK);
}

int init_work_thread()
{
    switch(pthread_create(&thread_connecting, NULL, connecting, NULL))
    {
        case 0:
        {
            write_log(NULL, "[Init] ok", 0);
            return 0;
        }
        case EAGAIN:
        {
            write_log(NULL, "[Init] Error: a system-imposed limit on the number of threads was encountered", 0);
            return EAGAIN;
        }
        default:
        {
            return -1;
        }
    }
}

int destroy_work_thread()
{
    if(pthread_cancel(thread_connecting) != 0)
    {
        write_log(NULL, "[Destroy] Error: cancel", 0);
    }
    switch(pthread_join(thread_connecting, NULL))
    {
        case 0:
        {
            write_log(NULL, "[Destroy] ok", 0);
            return 0;
        }
        case EDEADLK:
        {
            write_log(NULL, "[Destroy] Error: a deadlock was detected", 0);
            return EDEADLK;
        }
        case EINVAL:
        {
            write_log(NULL, "[Destroy] Error: thread is not a joinable thread", 0);
            return EINVAL;
        }
        case ESRCH:
        {
            write_log(NULL, "[Destroy] Erorr: thread not found", 0);
            return EINVAL;
        }
        default:
        {
            write_log(NULL, "[Destroy] Error", 0);
            return -1;
        }
    }
}

int my_daemon()
{
    int status = 0;

        struct sigaction sigact;
        // сигналы об ошибках в программе будут обрататывать более тщательно
        // указываем что хотим получать расширенную информацию об ошибках
        sigact.sa_flags = SA_SIGINFO;
        // задаем функцию обработчик сигналов
        sigact.sa_sigaction = signal_error;

        if(sigemptyset(&sigact.sa_mask) != 0)
        {
            write_log(NULL, "[DAEMON] Error: Failed to create empty signal handler", ERRNO_FLAG);
            return CHILD_NEED_TERMINATE;
        }

        // установим наш обработчик на сигналы

        if(sigaction(SIGFPE, &sigact, 0) != 0) // ошибка FPU
        {
            write_log(NULL, "DAEMON] Error: Failed to create handler of a SIGFPE signal", ERRNO_FLAG);
            return CHILD_NEED_TERMINATE;
        }
        if(sigaction(SIGILL, &sigact, 0) != 0) // ошибочная инструкция
        {
            write_log(NULL, "DAEMON] Error: Failed to create handler of a SIGILL signal", ERRNO_FLAG);
            return CHILD_NEED_TERMINATE;
        }
        if(sigaction(SIGSEGV, &sigact, 0) != 0) // ошибка доступа к памяти
        {
            write_log(NULL, "DAEMON] Error: Failed to create handler of a SIGSEGV signal", ERRNO_FLAG);
            return CHILD_NEED_TERMINATE;
        }
        if(sigaction(SIGBUS, &sigact, 0) != 0) // ошибка шины, при обращении к физической памяти
        {
            write_log(NULL, "DAEMON] Error: Failed to create handler of a SIGBUS signal", ERRNO_FLAG);
            return CHILD_NEED_TERMINATE;
        }

    sigset_t sigset;

    if(sigemptyset(&sigset) != 0)
    {
        write_log(NULL, "[DAEMON] Error: Failed to create empty blocker of a signal handler", ERRNO_FLAG);
        return CHILD_NEED_TERMINATE;
    }
    // блокируем сигналы которые будем ожидать

    // сигнал остановки процесса пользователем
    if(sigaddset(&sigset, SIGQUIT) != 0)
    {
        write_log(NULL, "[DAEMON] Error: Failed to add SIGQUIT to blocker", ERRNO_FLAG);
        return CHILD_NEED_TERMINATE;
    }
    // сигнал для остановки процесса пользователем с терминала
    if(sigaddset(&sigset, SIGINT) != 0)
    {
        write_log(NULL, "[DAEMON] Error: Failed to add SIGINT to blocker", ERRNO_FLAG);
        return CHILD_NEED_TERMINATE;
    }
    // сигнал запроса завершения процесса
    if(sigaddset(&sigset, SIGTERM) != 0)
    {
        write_log(NULL, "[DAEMON] Error: Failed to add SIGTERM to blocker", ERRNO_FLAG);
        return CHILD_NEED_TERMINATE;
    }
    if(sigprocmask(SIG_BLOCK, &sigset, NULL) != 0)
    {
        write_log(NULL, "[DAEMON] Error: Failed to apply blocker of a signal handler", ERRNO_FLAG);
        return CHILD_NEED_TERMINATE;
    }

    // Установим максимальное кол-во дискрипторов которое можно открыть
    set_fd_limit(FD_LIMIT);
    
    write_log(NULL, "[DAEMON] Started", 0);

    // запускаем потоки
    status = init_work_thread();
    if (!status)
    {
        int signo;

        // ждем указанных сообщений
        if(sigwait(&sigset, &signo) != 0)
        {
            write_log(NULL, "[DAEMON] Error of waiting a signal", ERRNO_FLAG);
        }
        else
        {
            char* str_sig = strsignal(signo);
            char* str = (char*)malloc(sizeof(char)*(strlen(str_sig)+strlen("[DAEMON] Signal ")+1));
            if(str == NULL)
            {
                write_log(NULL, "[DAEMON] Error: Failed to allocate memory", 0);
                write_log(NULL, "[DAEMON] Undefined signal", 0);
            }
            else
            {
                if(sprintf(str, "[DAEMON] Signal %s", str_sig) < 0)
                {
                    write_log(NULL, "[DAEMON] Error: fprintf failed", 0);
                }
                else
                {
                    write_log(NULL, str, 0);
                }
                free(str);
            }
        }
        
        // остановим все рабочие потоки
        destroy_work_thread();
    }
    else
    {
        write_log(NULL, "[DAEMON] Create work thread failed", 0);
    }

    write_log(NULL, "[DAEMON] Stopped", 0);
    return CHILD_NEED_TERMINATE;
}

int monitor()
{
    sigset_t sigset;

    // настраиваем сигналы которые будем обрабатывать
    int status = sigemptyset(&sigset);
    if(status != 0)
    {
        write_log(NULL, "[MONITOR] Error: Failed to create empty signal handler", ERRNO_FLAG);
        return status;
    }
    
    // сигнал остановки процесса пользователем
    status = sigaddset(&sigset, SIGQUIT);
    if(status != 0)
    {
        write_log(NULL, "[MONITOR] Error: Failed to add SIGQUIT to signal handler", ERRNO_FLAG);
        return status;
    }
    // сигнал для остановки процесса пользователем с терминала
    status = sigaddset(&sigset, SIGINT);
    if(status != 0)
    {
        write_log(NULL, "[MONITOR] Error: Failed to add SIGINT to signal handler", ERRNO_FLAG);
        return status;
    }
    // сигнал запроса завершения процесса
    status = sigaddset(&sigset, SIGTERM);
    if(status != 0)
    {
        write_log(NULL, "[MONITOR] Error: Failed to add SIGTERM to signal handler", ERRNO_FLAG);
        return status;
    }
    // сигнал посылаемый при изменении статуса дочернего процесса
    status = sigaddset(&sigset, SIGCHLD);
    if(status != 0)
    {
        write_log(NULL, "[MONITOR] Error: Failed to add SIGCHILD to signal handler", ERRNO_FLAG);
        return status;
    }
    status = sigprocmask(SIG_BLOCK, &sigset, NULL);
    if(status != 0)
    {
        write_log(NULL, "[MONITOR] Error: Failed to apply signal handler", ERRNO_FLAG);
        return status; 
    }

    // данная функция создаст файл с нашим PID'ом
    status = set_pid_file(pid_file);
    if(status != 0)
    {
        write_log(NULL, "[MONITOR] Error: Failed to set a PID file", 0);
        return status;
    }

    pid_t pid;
    int need_start = 1;
    while(1)
    {
        // если необходимо создать потомка
        if (need_start)
        {
            // создаём потомка
            pid = fork();
            need_start = 0;
        }
        
        if (pid == -1) // если произошла ошибка
        {
            write_log(NULL, "[MONITOR] Error: Fork failed ", ERRNO_FLAG);
            break;
        }
        else if (pid == 0) // если мы потомок
        {
            // запустим функцию отвечающую за работу демона
            status = my_daemon();
            exit(status);
        }
        // если мы родитель

        siginfo_t siginfo;

        // ожидаем поступление сигнала
        status = sigwaitinfo(&sigset, &siginfo);
        if(status < 0)
        {
            write_log(NULL, "[MONITOR] Error of waiting a signal", ERRNO_FLAG);
            break;
        }
        
        // если пришел сигнал от потомка
        if (siginfo.si_signo == SIGCHLD)
        {
            // получаем статус завершение
            if(wait(&status) < 0)
            {
                write_log(NULL, "[MONITOR] Error of waiting of a comletion code", ERRNO_FLAG);
                break;
            }
            
            // преобразуем статус в нормальный вид
            if(WIFEXITED(status))
                status = WEXITSTATUS(status);
            else
            {
                write_log(NULL, "[MONITOR] Child stopped with error", 0);
                status = -1;
                break;
            }
            // если потомок завершил работу с кодом говорящем о том, что нет нужды дальше работать
            if (status == CHILD_NEED_TERMINATE)
            {        
                write_log(NULL, "[MONITOR] Child stopped", 0);
                break;
            }
            else if (status == CHILD_NEED_WORK) // если требуется перезапустить потомка
            {
                write_log(NULL, "[MONITOR] Child restart", 0);
                need_start = 1;
            }
        }
        else // если пришел какой-либо другой ожидаемый сигнал
        {
            // запишем в лог информацию о пришедшем сигнале
            {
                char* str_sig = strsignal(siginfo.si_signo);
                char* str = (char*)malloc(sizeof(char)*(strlen(str_sig)+strlen("[MONITOR] Signal ")+1));
                if(str == NULL)
                {
                    write_log(NULL, "[MONITOR] Error: Failed to allocate memory", 0);
                    write_log(NULL, "[MONITOR] Undefined signal", 0);
                }
                else
                {
                    if(sprintf(str, "[MONITOR] Signal %s", str_sig) < 0)
                    {
                        write_log(NULL, "[MONITOR] Error: fprintf failed", 0);
                    }
                    else
                    {
                        write_log(NULL, str, 0);
                    }
                    free(str);
                }
            }
            
            // убьем потомка
            status = kill(pid, SIGTERM);
            if(status < 0)
            {
                write_log(NULL, "[MONITOR] Error: Failed to send SIGTERM to child", ERRNO_FLAG);
            }
            break;
        }
    }

    // запишем в лог, что мы остановились
    write_log(NULL, "[MONITOR] Stop", 0);
    
    // удалим файл с PID'ом
    delete_pid_file(pid_file);
    
    return status;
}

int main(int argc, char** argv)
{
    pid_t pid;
    pid_file = NULL;

    int interactive = 0;
    {
        char* log_file_name = NULL;
        for(int i = 1; i < argc; ++i)
        {
            if(strcmp(argv[i], "-l") == 0)
            {
                log_file_name = argv[++i];
            }
            else if(strcmp(argv[i], "-i") == 0)
            {
                interactive = 1;
            }
            else if(strcmp(argv[i], "-pid") == 0)
            {
                pid_file = argv[++i];
            }
            else if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            {
                printf("Usage: %s -l logfile -pid PID_file [-i]\n", argv[0]);
                return 0;
            }
            else
            {
                printf("Undefined key: %s\n", argv[i]);
                return 2;
            }
        }
        if ((log_file_name == NULL && interactive == 0) || pid_file == NULL)
        {
            printf("Usage: %s -l logfile -pid pid_file [-i]\n", argv[0]);
            return -1;
        }
        if(log_file_name != NULL)
            write_log(log_file_name, NULL, 0);
        if(interactive)
            write_log(NULL, NULL, interactive*INTERACTIVE_FLAG);
    
        // переходим в корень диска, если мы этого не сделаем, то могут быть проблемы.
        // к примеру с размантированием дисков
        if(chdir("/") != 0)
        {
            wrap_perror("Error changing directory")
            return 1;
        }
        //Проверяем доступность лог файла
        if(!interactive)
        {
            FILE* log = fopen(log_file_name, "a");
            if(log == 0)
            {
                wrap_perror("Error opening log file")
                //return 1;
            }
            else
            {
                if(fclose(log) == EOF)
                {
                    wrap_perror("Error closing log file")
                }
            }
        }
        //Проверяем доступность pid файла
        {
            FILE* pid_f = fopen(pid_file, "w");
            if(pid_f == 0)
            {
                wrap_perror("Error opening PID file")
                return 1;
            }
            else
            {
                if(fclose(pid_f) == EOF)
                {
                    wrap_perror("Error closing PID file")
                }
                else
                {
                    unlink(pid_file);
                }
            }
        }
    }

    if(check_pid_file(pid_file))
    {
        print_error("deamon is already running")
        return 3;
    }

    if(interactive)
    {
        umask(0);
        int status = monitor();
        write_log(NULL, NULL, MUTEX_DESTROY_FLAG);
        return status;
    }

    // создаем потомка
    pid = fork();

    if (pid == -1) // если не удалось запустить потомка
    {
        wrap_perror("Error: Start Daemon failed")        
        return 1;
    }
    else if (pid == 0) // если это потомок
    {
        int status;        
        // разрешаем выставлять все биты прав на создаваемые файлы,
        // иначе у нас могут быть проблемы с правами доступа
        umask(0);
        
        // создаём новый сеанс, чтобы не зависеть от родителя
        if(setsid() == -1)
        {
            write_log(NULL, "Error setsid", ERRNO_FLAG);
            return -1;
        }

        // закрываем дискрипторы ввода/вывода/ошибок, так как нам они больше не понадобятся
        if(close(STDIN_FILENO) < 0)
        {
            write_log(NULL, "[START] Error: Failed to close stdin", ERRNO_FLAG);
        }
        if(close(STDOUT_FILENO) < 0)
        {
            write_log(NULL, "[START] Error: Failed to close stdout", ERRNO_FLAG);
        }
        if(close(STDERR_FILENO) < 0)
        {
            write_log(NULL, "[START] Error: Failed to close stderr", ERRNO_FLAG);
        }
        
        // Данная функция будет осуществлять слежение за процессом
        status = monitor();
        write_log(NULL, NULL, MUTEX_DESTROY_FLAG);
        return status;
    }
    else // если это родитель
    {
        // завершим процес, т.к. основную свою задачу (запуск демона) мы выполнили
        return 0;
    }
}