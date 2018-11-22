#include<stdio.h>
#include<stdlib.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<errno.h>
#include<pthread.h>
#include<string.h>
#include<signal.h>
#include<unistd.h>
#include<sys/ipc.h>
#include<semaphore.h>
#include<sys/shm.h>
#include<fcntl.h>
#include<semaphore.h>
#define MAX_P 20
#define MAX_T 100
#define KEY 98765
#define SERVER_PORT 5000

volatile int flag = 0; //0 if parent process, 1 if new child spawned... used to break out of infinite loop in main
int identity; //process identity
int done = 0; //have cpp clients been processed?

struct thread_node{
	int thread_count;
	int thread_conn;
	pthread_t thread_id;
	char client_name[100];
}Thread_node; //connection data for each thread

struct process_node{
	struct thread_node thread_connfd[MAX_T];
	int pid;
}Process_node; //thread data for each process

socklen_t addr_len;
int Nprocesses; //n
int Nthreads; //t
int Nclients; //current clients (<=t)
int Nprocessed;
int MAX_CLIENTS; //cpp
int shmid,shmid2,shmid3; 
struct process_node *processes; //shared memory array for Process_node
int *id; //shared memory for assigning identity to new process spawned
sem_t *wmutex; //shared memory mutex
int listenfd;
pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER; //lock for functions to access thread_node
pthread_mutex_t access_mutex = PTHREAD_MUTEX_INITIALIZER;//lock for main_thread to accept new connections using listenfd
pthread_t thread_id;


void exit_handler(int signum){ //Handle end of a process, both forced and upon completion of cpp requests
	int i;
	printf("\n\nAll the Threads have been terminated. All data structures cleaned.\nProcess %d Shutting Down. Thank you for using Old-School Chat.\n\n", identity);
    fflush(stdout);
        
	for(i=0;i<Nthreads;i++){
		if(processes[identity].thread_connfd[i].thread_count == 1){
			pthread_kill(processes[identity].thread_connfd[i].thread_id, SIGKILL);
			processes[identity].thread_connfd[i].thread_count = 0;
			processes[identity].thread_connfd[i].thread_id = -1;
			sprintf(processes[identity].thread_connfd[i].client_name,"null");
		}
	}
	close(listenfd);
	if(signum==-1){
		*id = identity;
		kill(getppid(), SIGUSR1);
	}
	
	exit(0);
}

void main_exit(int signum){ //Handle end of main parent process... detach and remove all shared memory segments
	sem_close(wmutex);
	shmdt(processes);
	shmdt(id);
	shmdt(wmutex);
	shmctl(shmid, IPC_RMID, NULL);
	shmctl(shmid2, IPC_RMID, NULL);
	shmctl(shmid3, IPC_RMID, NULL);
	
	printf("Exiting Server...\n");
	exit(0);
}

void handler1(int signum) //Main parent process spawns new process after a process completes cpp requests
{
	printf("new process\n");
	if(fork()==0)
		flag = 1;

	identity = *id;
}

void handler2(int signum) //Used to read the chat message written by the process connected to chat sender by the process connected to chat reciever
{
	//printf("in handler\n");
	FILE *fp = fopen("temp.txt", "r");
	char *line = NULL, *msg;
	int fd;
	ssize_t len;
	getline(&line, &len, fp);
	msg = (char*) malloc((len-2)*sizeof(char));
	strcpy(msg, line+2);
	fd = atoi(strtok(line, " "));
	//printf("%d;%s\n", fd, msg);
	fclose(fp);
	unlink("temp.txt");
	write(fd, msg, strlen(msg));
	//sem_getvalue(&wmutex, &sem_val);
	//printf("mutex val : %d\n", sem_val);
	sem_post(wmutex);
}

int print_err(int connfd){
	char error_msg[] = "Sorry, the command you entered is not supported by the server.\n";
	write(connfd, error_msg, strlen(error_msg));	
	printf("%s\n",error_msg);
	return 0;
}

int join(int connfd, char *cmd, int index){ //process join request of client
	if(*cmd != ' '){
		char error_msg[] = "Sorry, the command you entered is not supported by the server.\nFORMAT FOR JOIN: JOIN <name>\n";
	        write(connfd, error_msg, strlen(error_msg));
		printf("%s", error_msg);
		return 0;	
	}
	char *name = (cmd+1);
	//access to shared data
	pthread_mutex_lock(&access_mutex);
	if(processes[identity].thread_connfd[index].thread_count == 1){
    	if(strcmp(processes[identity].thread_connfd[index].client_name, name) == 0){
    		char error_msg[] = "That is already your name!\nEnter a different command.\n";
	        write(connfd, error_msg, strlen(error_msg));
			printf("%s", error_msg);
			pthread_mutex_unlock(&access_mutex);
			return 0;
		}
	}
    		 	
	int i,j;
	for(j=0;j<Nprocesses;j++){
		for(i=0;i<Nthreads;i++){
        	if(processes[j].thread_connfd[i].thread_count == 1){
                if(strcmp(processes[j].thread_connfd[i].client_name, name) == 0){
                	char error_msg[] = "Sorry, the name entered by you is already taken!\nPlease try a different name.\n";
	        		write(connfd, error_msg, strlen(error_msg));
					printf("%s", error_msg);
					pthread_mutex_unlock(&access_mutex);
					return 0;
				}
			}
		}
	}
        
    processes[identity].thread_connfd[index].thread_count= 1;                	
	sprintf(processes[identity].thread_connfd[index].client_name, "%s", name);
	pthread_mutex_unlock(&access_mutex);
	char resp[1000];
	char response[] = "Your nick name for the chat is: ";
	sprintf(resp, "%s%s\n", response, cmd+1);
	printf("%s",resp);
	write(connfd, resp, strlen(resp));
	return 0;
}

int chat(int connfd, char *cmd, int index){ //process chat request of client, if chat reciever in same process... directly write to fd; else send chat message to the process connected to chat reciever
        if(*cmd != ' '){
                char error_msg[] = "Sorry, the command you entered is not supported by the server.\nFORMAT FOR CHAT (User Message): CHAT <name> <message>\n";
                write(connfd, error_msg, strlen(error_msg));
                printf("%s", error_msg);
                return 0;
        }
	char *tname = strtok(cmd+1, " ");
	int offset = strlen(tname) + 2;
	char *msg = cmd + offset;

	char msg_to_usr[1000];
	pthread_mutex_lock(&access_mutex);
	sprintf(msg_to_usr, "%s: %s\n", processes[identity].thread_connfd[index].client_name, msg);
	char sent[] = "Message sent. :)\n";
	char failed[] = "Message failed to send. User requested is offline. :(\n";
	
        printf("%s", msg_to_usr);
        int i,j;
	for(j=0;j<Nprocesses;j++){
		for(i=0;i<Nthreads;i++){
                if(processes[j].thread_connfd[i].thread_count == 1){
                        if(strcmp(processes[j].thread_connfd[i].client_name, tname) == 0){
				//printf("send fd : %d, process: %d\n", processes[j].thread_connfd[i].thread_conn, j);
				if(j==identity)
					write(processes[j].thread_connfd[i].thread_conn, msg_to_usr, strlen(msg_to_usr));
				else{
					//printf("here\n");
					sem_wait(wmutex);
					FILE *fp = fopen("temp.txt", "w");
					//sprintf(string, "%d %s", processes[j].thread_connfd[i].thread_conn, msg_to_usr);
					fprintf(fp, "%d %s", processes[j].thread_connfd[i].thread_conn, msg_to_usr);
					fclose(fp);
					kill(processes[j].pid, SIGUSR2);
				}

				write(connfd, sent, strlen(sent));
				break;
			}
		}
    }
    
	if(i!=Nthreads)
		break;
	}
	
	pthread_mutex_unlock(&access_mutex);

    if(j == Nprocesses)
		write(connfd, failed, strlen(failed));	
	
	return 0;
}

int leave(int connfd, int index){ //process leave request of client
    pthread_mutex_lock(&access_mutex);
	strcpy(processes[identity].thread_connfd[index].client_name,"");
	processes[identity].thread_connfd[index].thread_count = 0;
	processes[identity].thread_connfd[index].thread_conn = -1;
	pthread_mutex_unlock(&access_mutex);
	char exit_msg[] = "Thank you for joining the Old-School Chat! You are Welcome anytime. It's free and always will be.\n";
	write(connfd, exit_msg, strlen(exit_msg));
	printf("%s", exit_msg);
	printf("Clients : %d\n", Nclients);
	fflush(stdout);
	if(Nclients<=0 && done==1){	
		printf("\nMaximum number of Clients (= %d) processed. Process Exiting... ", MAX_CLIENTS);
		exit_handler(-1);
	}
	return 1;
}

int process_cmd(char *cmd, int connfd, int index){ //parse the command of client and invoke required function
	int n = 0;
	if(cmd == NULL){
		return 0;
	}
	if(strncmp(cmd, "JOIN", 4)==0 || strncmp(cmd, "join", 4)==0){
		if(processes[identity].thread_connfd[index].thread_count<=0)
			Nclients++;
			
		n = join(connfd, cmd+4, index);
	}
	else if(processes[identity].thread_connfd[index].thread_count<=0 && (strncmp(cmd, "LEAV", 4)==0 || strncmp(cmd, "leav", 4)==0)){
		n = leave(connfd, index);
		//printf("left\n");
	}
	else{
		if(strlen(processes[identity].thread_connfd[index].client_name)>0){
			if((strncmp(cmd, "CHAT", 4)==0 || strncmp(cmd, "chat", 4)==0)){
				n = chat(connfd, cmd+4, index);
			}
			else if((strncmp(cmd, "LEAV", 4)==0 || strncmp(cmd, "leav", 4)==0)){
				Nprocessed++;
				Nclients--;	
				if(Nprocessed>=MAX_CLIENTS){
					close(listenfd);
					done = 1;
				}
				n = leave(connfd, index);
			}
			else{
				n = print_err(connfd);
			}
		}
		else{
			char pls_join[1000];
			sprintf(pls_join, "You have not joined the Old-School ChatServer. Please join the chat to continue. Join using following command:\nJOIN <name>\n");
			write(connfd, pls_join, strlen(pls_join));
			return 0;
		}
	}
	return n;
		
}

void process(int connfd, int index){ //listen for new commands from client
	char buf[100];
	char *cmd;
	int n;
	int ret_val = 0;
	while(n = read(connfd, buf, sizeof(buf)-1)){
		buf[n] = '\0';
		printf("Process : %d. BUF READ: %s\n N = %d\n", identity, buf,n);
		cmd = strtok(buf,"\r\n");
		printf("Process : %d. CMD 1 %s\n", identity, cmd);
		ret_val = process_cmd(cmd,connfd,index);
		memset(buf,0,sizeof(buf));
		if(ret_val == 1){
			return;
		}
	}
	return;
}

void *thread_main(void *arg){ //start function of a thread... accepts a connection from client and invokes process
	//thread executing here.
	char welcome[1000];
	sprintf(welcome, "\nWelcome to Old-School Chat! The following are instructions to get you started.\n1. Join Chat: JOIN <name>\n2. Send Message: CHAT <username> <message to send>\n3. Leave Chat: LEAV\n\nIt's free and always will be.\n\n"); 
	int connfd;
	int thread_number = *((int *) arg);
	printf("Process : %d. In the thread: %d:: %d\n", identity, (int) pthread_self(), thread_number);
	socklen_t client_addr_len;
	struct sockaddr *client_addr;
	client_addr = (struct sockaddr *) malloc(addr_len);
	printf("Process : %d. Listen FD: %d, Len: %d\n", identity, (int)listenfd, addr_len);
	for(;;){
		pthread_mutex_lock(&thread_lock);
		if(done==1)
			break;
		connfd = accept(listenfd, client_addr, &addr_len);
		if(connfd == -1){
			printf("Process : %d. Cannot accept.\n", identity);
			exit(1);
		}
		printf("Process : %d. Accepted\n", identity);
		pthread_mutex_unlock(&thread_lock);
		pthread_mutex_lock(&access_mutex);
		processes[identity].thread_connfd[thread_number].thread_count = 0;
		processes[identity].thread_connfd[thread_number].thread_conn = connfd;
		processes[identity].thread_connfd[thread_number].thread_id = pthread_self();
		pthread_mutex_unlock(&access_mutex);
		write(connfd, welcome, strlen(welcome));
		process(connfd,thread_number);
		close(connfd);
	}	
}

void make_thread(int i){ //spawning of a thread
	//create a thread in this function.
	printf("Process : %d(%d). Creating Thread Number: %d.\n", identity, processes[identity].pid, i);	
	int *temp_ptr = (int *) malloc(sizeof(int));
	*temp_ptr = i;
	pthread_create(&thread_id, NULL, &thread_main, temp_ptr);
	return;
}

int make_process(int t, int cpp) //main function with respect to a process, creates t threads, i.e., invokes make_thread() t times
{
	addr_len = sizeof(struct sockaddr);
	Nthreads = t;
	//SERVER_PORT = atoi(argv[2]);
        MAX_CLIENTS = cpp;
	Nclients = 0;
	Nprocessed = 0;
	processes[identity].pid = getpid();
	printf("\n\n\nServer process : %d. \n**********************RUNNING INSTRUCTIONS FOR CLIENT******************************\nRun this command on a seperate terminal: telnet localhost %d\n\n", 	    identity, SERVER_PORT);
	printf("Process : %d. Number of threads: %d\n", identity, Nthreads);
        printf("Process : %d. Maximum Clients: %d\n", identity, MAX_CLIENTS);
        //fflush(stdout);

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	signal(SIGINT, exit_handler);
	signal(SIGQUIT, exit_handler);
	signal(SIGTSTP, exit_handler);
    signal(SIGTERM, exit_handler);

	if(listenfd == -1){
		printf("Sorry, cannot create a socket listenfd.\n");
		exit(1);
	}
	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(SERVER_PORT);
	
	int option =1;
	setsockopt(listenfd,SOL_SOCKET,(SO_REUSEPORT | SO_REUSEADDR),(char*)&option,sizeof(option));
	
	if(bind(listenfd,(struct sockaddr *) &server_addr,sizeof(server_addr) )==-1){
		printf("ERROR: %s\n", strerror(errno));
		exit(1);
	}
	listen(listenfd, MAX_T);
	//create Nthreads;
	int i;
	for(i=0;i<Nthreads;i++){
		make_thread(i);	
	}
	for(;;){
		//pause();
	}
	return 0;
}

void
main(int argc, char **argv) // main function of chat server
{
	if(argc!=4)
		perror("invalid CLA");

	signal(SIGUSR1, handler1);
	signal(SIGUSR2, handler2);

	int i, n = atoi(argv[1]), t = atoi(argv[2]), cpp = atoi(argv[3]), pid;
 	if(n>20 || t>100)
		perror("invalid CLA");

	Nprocesses = n;

	shmid = shmget(KEY, MAX_P*sizeof(struct process_node), IPC_CREAT | 0666);
	shmid2 = shmget(KEY*2, sizeof(int*), IPC_CREAT | 0666);
	shmid3 = shmget(KEY*3, sizeof(sem_t*), IPC_CREAT | 0666);
	processes = (struct process_node*) shmat(shmid,0,0);
	id = (int*) shmat(shmid2,0,0);
	wmutex = (sem_t*) shmat(shmid3, 0, 0);
	
	*id = -1;
	//printf("%d", *id);

	sem_init(wmutex, 1, 1);
	signal(SIGINT, main_exit);
	signal(SIGQUIT, main_exit);
	signal(SIGTSTP, main_exit);
    signal(SIGTERM, main_exit);

	for(i=0;i<n;i++){
		(*id)++;
		identity = i;

		if((pid=fork())<0)
			perror("fork error");

		else if(pid==0)
			break;
	}

	if(i==n){
		while(1){
			if(flag==1)
				break;
		}
	}

	make_process(t, cpp);
}

