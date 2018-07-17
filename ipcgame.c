/*
    A kind of racing "game" for getting familiar with IPC.

    Child processes spawn and send messages to a watchdog-
    process through a message queue. The watchdog recieves
    the messages and increments the score of the client
    that sent the message. The first client whose messages
    have all been consumed by the watchdog, has won. The 
    watchdog then sends SIGUSR1 to all childs. The handler
    then shuts them down.

    All processes share memory to always have up to date
    information wich client is the first in the race atm.
    Writing in the shared memory is getting controlled by 
    a binary semaphore.
*/

# include <stdio.h>
# include <stdlib.h> // exit()
# include <sys/msg.h> // message-queue
# include <sys/sem.h> // semaphores
# include <sys/shm.h> // shared mem
# include <sys/wait.h> // waitpid
# include <unistd.h> // fork
# include <signal.h> // sigaction, ...
# include <string.h>

# define MAXCLIENTS 12
# define BUFSIZE 100
# define MSGNR 100
# define SERVER 1
# define LOCK -1
# define UNLOCK 1
# define MAXRUNS (int)(MSGNR*MAXCLIENTS)

// ID's for IPC-objects as shared constants
int SMgame=0;
int WatchdogMQ=0;
int SEMaccess=0;

// for shared-mem SMgame
typedef struct{
    int frontrunner;
    int runs_done;
    int clientpids[MAXCLIENTS];
} game;

// for msg-queue WatchdogMQ
typedef struct{
    long msg_type;
    char data[BUFSIZE];
} message_t;

void watchdog_code();
void client_code(int);
static void SIGHandler(int);
int sem_operation(int);
void random_sleep();

int main() {
    int i=0;
    pid_t pid, watchdog_pid;
    game *game_main;

    // create MQ
    if ((WatchdogMQ=msgget(IPC_PRIVATE, 0660)) < 0){
        perror("msgget");
        exit(EXIT_FAILURE);
    } else{
        printf("WatchdogMQ initialized\n");
    }

    // create SEMaccess
    if ((SEMaccess=semget(IPC_PRIVATE, 1, 0660)) < 0){
        perror("semget");
        exit(EXIT_FAILURE);
    } else{ 
        // init with 1
        if (semctl(SEMaccess, 0, SETVAL, 1) < 0){
            perror("semctl");
            exit(EXIT_FAILURE);
        } else{
            printf("SEMaccess initialized\n");
        }
    }

    // create shared-mem
    if ((SMgame=shmget(IPC_PRIVATE, sizeof(game), 0660)) < 0){
        perror("shmget");
        exit(EXIT_FAILURE);
    } else{
        if ((game_main=shmat(SMgame, NULL, SHM_R|SHM_W)) < 0){
            perror("shmat");
            exit(EXIT_FAILURE);
        } else{
            game_main->runs_done=0;
            printf("SMgame initialized\n");
        }
    }

    // fork watchdog-proc
    switch (watchdog_pid=fork()){
        case 0: // child-proc
            watchdog_pid = getpid();
            watchdog_code();
            exit(EXIT_SUCCESS);
            break;
        case -1: // err
            perror("fork");
            exit(EXIT_FAILURE);
            break;
        case 1: break;
    }

    fflush(stdout);

    // fork client-procs
    printf("-----forking-----\n");
    for (i=0;i<MAXCLIENTS;i++){
        switch (pid=fork()){
            case 0: // client-proc
                sem_operation(LOCK);
                game_main->clientpids[i]=getpid();
                sem_operation(UNLOCK);

                random_sleep();
                client_code(i);
                exit(EXIT_SUCCESS);
            case -1: // err
                perror("fork");
                exit(EXIT_FAILURE);
            case 1: break;
        }
    }

    sleep(2); // prevent race condition
    while (wait(NULL) < 0); // wait for clients to finish

    // delete IPC objects
    fflush(stdout);
    printf("---clients-fin----\n");
    printf("cleaning up\n");
    if ((msgctl(WatchdogMQ, IPC_RMID, 0)) < 0){
        perror("msgctl");
    } else{
        printf("WatchdogMQ deleted\n");
    }
    if ((semctl(SEMaccess, 0, IPC_RMID)) < 0){
        perror("semctl");
    } else{
        printf("SEMaccess deleted\n");
    }
    if ((shmctl(SMgame, IPC_RMID, NULL)) < 0){
        perror("shmctl");
    } else{
        printf("SMgame deleted\n");
    }

} // main()

void watchdog_code(){
    printf("[watchdog] starting with pid %d\n", getpid());
    int scores[MAXCLIENTS]={0};
    int i;
    int frontrunner=0, winner=-1;

    // attach SM
    game* watchdog_client;
    if ((watchdog_client=(game*)shmat(SMgame, NULL, SHM_R|SHM_W)) < 0){
        perror("[watchdog] shmat");
        return; // cant continue without access
    }

    message_t msg;
    int client_nr;

    do { // recieve msg
        if (msgrcv(WatchdogMQ, &msg, sizeof(msg.data), (long) SERVER, 0) < 0){
            perror("[watchdog] msgrcv");
            exit(EXIT_FAILURE);

        } else { // get client nr out of msg
            if (sscanf(msg.data, "%d", &client_nr) < 0){
                perror("[watchdog] sscanf");
                continue;

            } else {
                scores[client_nr]++;

                // check who is frontrunner atm
                int frontrunner_score=0;
                for (i=0;i<MAXCLIENTS;i++){
                    if (scores[i] > frontrunner_score){
                        frontrunner_score = scores[i];
                        frontrunner = i;
                    }
                }

                if (scores[frontrunner]%5==0 && client_nr==frontrunner){
                    printf("[watchdog] client %d first to get to %d points\n", client_nr, scores[client_nr]);
                }

                if (scores[frontrunner]==MSGNR && client_nr==frontrunner){
                    winner = frontrunner;
                }

                // write frontrunner into SM
                sem_operation(LOCK);
                watchdog_client->frontrunner = frontrunner;
                watchdog_client->runs_done++;              
                sem_operation(UNLOCK);
            }
        }
    } while (winner == -1 && watchdog_client->runs_done < MAXRUNS);

    printf("[watchdog] finished, winner: client %d\n", winner);

    // SIGUSR 1 to clients
    for (i=0;i<MAXCLIENTS;i++){
        kill(watchdog_client->clientpids[i], SIGUSR1);
    }
}

void client_code(int nr){
    int i=0;
    message_t message;
    char buf[BUFSIZE], err[BUFSIZE];
    printf("[client %d] starting with pid %d\n", nr, getpid());

    // register SIGUSR1-handler
    struct sigaction sa = {SIGHandler, SIGUSR1, 0};
    if ((sigaction(SIGUSR1, &sa, NULL)) < 0){
        char buf[BUFSIZE];
        snprintf(buf, sizeof(buf), "sigaction client %d", nr);
        perror(buf);
    }

    // attach shared mem
    game* game_main;
    if ((game_main=shmat(SMgame, NULL, SHM_R|SHM_W)) < 0){
        perror("shmat");
        exit(EXIT_FAILURE);
    }
    
    // prepare message-string
    snprintf(buf, sizeof(buf), "%d", nr);
    message.msg_type=SERVER;
    strcpy(message.data, buf);

    // send messages
    int frontrunner=0; int actual_frontrunner=0;
    for (i=0;i<MSGNR;i++){

        random_sleep();

        if ((msgsnd(WatchdogMQ, &message, sizeof(message.data), 0)) < 0){
            snprintf(err, sizeof(err), "msgsnd c%d", nr);
            perror(err);

        } else {

            // check for new frontrunner after each msg
            sem_operation(LOCK);
            actual_frontrunner = game_main->frontrunner;
            sem_operation(UNLOCK);

            if (frontrunner != actual_frontrunner){
                frontrunner = actual_frontrunner;
            }
        }
    }

    printf("[client %d] finished\n", nr);
}

int sem_operation(int op){
    // helper-function for locking/unlocking the semaphre

    static struct sembuf sbuf;
    sbuf.sem_op = op;
    sbuf.sem_flg = SEM_UNDO;

    if (semop(SEMaccess, &sbuf, 1) < 0){
        perror("semop");
        return -1;
    }
    return 1;
}

void random_sleep(){
     // add a little randomness
    srand(getpid()%1234);
    usleep(rand()%398);
    return;
}

static void SIGHandler(int signr){
    // SIGUSR1
    exit(EXIT_SUCCESS);
}